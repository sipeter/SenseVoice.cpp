#include "audio_pipe.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// 帧协议常量（与 stream.cc 保持一致）
static const uint8_t FRAME_TYPE_CMD = 0x01;
static const uint8_t FRAME_TYPE_AUDIO = 0x02;

// 全局控制信号（在 stream.cc 中定义）
extern std::atomic<bool> g_is_recording;
extern std::atomic<bool> g_should_exit;
extern std::atomic<bool> g_needs_flush;

audio_pipe::audio_pipe(int sample_rate) 
    : m_sample_rate(sample_rate), m_running(false) {
}

audio_pipe::~audio_pipe() {
    m_running = false;
    if (m_reader.joinable()) {
        m_reader.join();
    }
}

bool audio_pipe::init() {
    if (m_running) return true;
    
#ifdef _WIN32
    // Windows: 将 stdin 设置为二进制模式
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    m_running = true;
    m_reader = std::thread(&audio_pipe::reader_thread, this);
    
    std::cerr << "[audio_pipe] 已启动帧协议读取线程" << std::endl;
    return true;
}

void audio_pipe::reader_thread() {
    std::cerr << "[audio_pipe] 线程开始运行" << std::endl;
    
    while (m_running && !g_should_exit) {
        // 1. 读取帧类型（1字节）
        uint8_t frame_type;
        if (fread(&frame_type, 1, 1, stdin) != 1) {
            if (feof(stdin)) {
                std::cerr << "[audio_pipe] stdin 已关闭" << std::endl;
                g_should_exit = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        
        // 2. 读取帧长度（4字节 Little-Endian）
        uint32_t frame_len;
        if (fread(&frame_len, 4, 1, stdin) != 1) {
            std::cerr << "[audio_pipe] 读取帧长度失败" << std::endl;
            continue;
        }
        
        // 安全检查
        if (frame_len > 1024 * 1024) { // 最大 1MB
            std::cerr << "[audio_pipe] 帧长度异常: " << frame_len << std::endl;
            continue;
        }
        
        // 3. 读取帧数据
        std::vector<uint8_t> data(frame_len);
        size_t total_read = 0;
        while (total_read < frame_len) {
            size_t bytes_read = fread(data.data() + total_read, 1, frame_len - total_read, stdin);
            if (bytes_read == 0) {
                if (feof(stdin)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            total_read += bytes_read;
        }
        
        if (total_read != frame_len) {
            std::cerr << "[audio_pipe] 帧数据读取不完整" << std::endl;
            continue;
        }
        
        // 4. 根据帧类型处理
        if (frame_type == FRAME_TYPE_CMD) {
            // 文本指令
            std::string cmd(data.begin(), data.end());
            // 去除换行符
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) {
                cmd.pop_back();
            }
            
            std::cerr << "[audio_pipe] 收到指令: " << cmd << std::endl;
            
            if (cmd == "START") {
                g_is_recording = true;
                g_needs_flush = false;
            } else if (cmd == "STOP") {
                g_is_recording = false;
                g_needs_flush = true;
            } else if (cmd == "EXIT") {
                g_should_exit = true;
                break;
            }
        }
        else if (frame_type == FRAME_TYPE_AUDIO) {
            // 音频数据：PCM 16-bit
            size_t sample_count = frame_len / 2;
            push_pcm16(reinterpret_cast<const int16_t*>(data.data()), sample_count);
        }
    }
    
    m_running = false;
    std::cerr << "[audio_pipe] 线程结束" << std::endl;
}

void audio_pipe::push_pcm16(const int16_t* data, size_t sample_count) {
    if (sample_count == 0) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 转换 int16 -> float [-1, 1]
    for (size_t i = 0; i < sample_count; ++i) {
        float sample = static_cast<float>(data[i]) / 32768.0f;
        m_buffer.push_back(sample);
    }
    
    // 限制缓冲区大小（最多保留 30 秒）
    const size_t max_samples = m_sample_rate * 30;
    if (m_buffer.size() > max_samples) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + (m_buffer.size() - max_samples));
    }
}

void audio_pipe::get(int ms, std::vector<float>& audio) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t samples_needed = (m_sample_rate * ms) / 1000;
    
    if (m_buffer.size() >= samples_needed) {
        audio.assign(m_buffer.begin(), m_buffer.begin() + samples_needed);
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + samples_needed);
    } else if (m_buffer.size() > 0) {
        audio.assign(m_buffer.begin(), m_buffer.end());
        m_buffer.clear();
    } else {
        audio.clear();
    }
}

void audio_pipe::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.clear();
}
