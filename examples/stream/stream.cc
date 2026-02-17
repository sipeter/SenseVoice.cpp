/*
 * Modified stream.cc for Windows Offline Voice Input (Geek Edition)
 * 包含功能：
 * 1. 无限长语音支持 (Smart Segmentation)
 * 2. 耳语增强 (AGC)
 * 3. [新增] 调试音频保存 (Save debug.wav)
 */

#include "common-sdl.h"
#include "common.h"
#include "sense-voice.h"
#include "silero-vad.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include <iostream>
#include <cmath> // used for AGC
#include <ctime> // for timestamp
#include <memory>
#include "../zian_services.h"
#include "../audio_pipe.h" // Zian Link: 支持 stdin PCM 输入
#include "../audio_socket.h" // Zian Link: 支持 socket PCM 输入
#include "../ring_buffer.h"

// ==========================================
// 全局控制信号
// ==========================================
std::atomic<bool> g_is_recording(false);
std::atomic<bool> g_should_exit(false);
std::atomic<bool> g_needs_flush(false);
std::atomic<bool> g_preempt_requested(false);
std::atomic<int> g_active_source(0);

enum class SourceKind {
    None = 0,
    PC = 1,
    Phone = 2,
};

// 【Zian Link】帧协议常量
const uint8_t FRAME_TYPE_CMD = 0x01;   // 文本指令帧
const uint8_t FRAME_TYPE_AUDIO = 0x02; // 音频数据帧

// ==========================================
// 音频文件保存辅助函数 (用于调试)
// ==========================================

// 复用已有的 wav 转换逻辑
void float_to_pcm16(const std::vector<float>& float_audio, std::vector<int16_t>& pcm16_audio) {
    pcm16_audio.resize(float_audio.size());
    for (size_t i = 0; i < float_audio.size(); ++i) {
        float sample = std::max(-1.0f, std::min(1.0f, float_audio[i]));
        pcm16_audio[i] = static_cast<int16_t>(sample * 32767.0f);
    }
}

struct WAVHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;
    uint16_t num_channels = 1;
    uint32_t sample_rate = SENSE_VOICE_SAMPLE_RATE;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
    WAVHeader() {
        byte_rate = sample_rate * num_channels * bits_per_sample / 8;
        block_align = num_channels * bits_per_sample / 8;
        file_size = 0; data_size = 0;
    }
};

void write_wav_file(const std::string& filename, const std::vector<float>& audio_data, int sample_rate) {
    if (audio_data.empty()) return;

    std::vector<int16_t> pcm16;
    float_to_pcm16(audio_data, pcm16);

    WAVHeader header;
    header.sample_rate = sample_rate;
    header.byte_rate = header.sample_rate * 2; // 16bit = 2 bytes
    header.data_size = (uint32_t)pcm16.size() * 2;
    header.file_size = header.data_size + 36;

    std::ofstream file(filename, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(pcm16.data()), pcm16.size() * 2);
        file.close();
        // 只有在 debug 模式或为了确认时才打印，以免污染 IPC 通道，
        // 但这里我们加上前缀 DEBUG: 方便 C# 过滤
        std::cout << "[[DEBUG: Saved " << filename << "]]" << std::endl;
    }
}

// ==========================================
// 耳语增强 (AGC) 实现 - 已移除
// ==========================================
// 恢复原始音质，依靠模型原生能力


// ==========================================
// 辅助结构与函数 (参数解析等)
// ==========================================
struct sense_voice_stream_params {
    int32_t n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t n_processors = 1;
    int32_t capture_id = -1;
    int32_t chunk_size = 50; // ms
    bool use_gpu = true;
    bool flash_attn = false;
    bool debug_mode = false;
    bool use_vad = false;
    bool use_itn = false;
    bool use_prefix = false;
    // 【新增】默认关闭，只有传入 --save-audio 才开启，用于调试的时候，保存wav文件
    bool save_audio = false;
    // 【Zian Link】从 stdin 读取 PCM 音频而非麦克风
    bool stdin_audio = false;
    // 【Zian Link】从 socket 读取 PCM 音频而非麦克风
    bool socket_audio = false;
    // 【Zian Link】双输入：本地麦克风 + socket 同时启用（Selection 模式）
    bool dual_audio = false;
    int socket_port = 8182;
    std::string audio_path = "debug.wav";
    std::string language = "auto";
    std::string model = "models/ggml-base.en.bin";
    float speech_prob_threshold = 0.1f;
};

static bool get_stream_params(int argc, char **argv, sense_voice_stream_params &params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-t" || arg == "--threads") params.n_threads = std::stoi(argv[++i]);
        else if (arg == "-c" || arg == "--capture") params.capture_id = std::stoi(argv[++i]);
        else if (arg == "-l" || arg == "--language") params.language = argv[++i];
        else if (arg == "-m" || arg == "--model") params.model = argv[++i];
        else if (arg == "-ng" || arg == "--no-gpu") params.use_gpu = false;
        else if (arg == "-fa" || arg == "--flash-attn") params.flash_attn = true;
        else if (arg == "--use-itn") params.use_itn = true;
        else if (arg == "--use-vad") params.use_vad = true;
        // 【Zian Link】从 stdin 读取 PCM 音频
        else if (arg == "--stdin-audio") params.stdin_audio = true;
        // 【Zian Link】从 socket 读取 PCM 音频（可选端口）
        else if (arg == "--socket-audio") {
            params.socket_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                params.socket_port = std::stoi(argv[++i]);
            }
        }
        // 【Zian Link】双输入模式（可选端口）
        else if (arg == "--dual-audio") {
            params.dual_audio = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                params.socket_port = std::stoi(argv[++i]);
            }
        }
        // 【新增】解析保存音频的参数 (支持可选路径)
        else if (arg == "--save-audio") {
            params.save_audio = true;
            // 检查下一个参数是否是路径 (不以 - 开头)
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                params.audio_path = argv[++i];
            }
        }
    }
    return true;
}

static void sense_voice_free_stream(struct sense_voice_context *ctx) {
    if (ctx) {
        ggml_free(ctx->model.ctx);
        ggml_backend_buffer_free(ctx->model.buffer);
        delete ctx->model.model->encoder;
        delete ctx->model.model;
        delete ctx;
    }
}

// ==========================================
// 双输入：SDL 麦克风采集封装（带 RingBuffer）
// ==========================================
class sdl_mic_source {
public:
    explicit sdl_mic_source(int chunk_ms)
        : m_chunk_ms(chunk_ms), m_audio(nullptr), m_running(false),
          m_buffer(static_cast<size_t>(SENSE_VOICE_SAMPLE_RATE) * 30) {
    }

    bool init(int capture_id, int sample_rate) {
        m_audio.reset(new audio_async(m_chunk_ms << 2));
        if (!m_audio->init(capture_id, sample_rate)) {
            return false;
        }
        if (!m_audio->resume()) {
            return false;
        }
        return true;
    }

    void start() {
        if (m_running) return;
        m_running = true;
        m_thread = std::thread([this]() {
            std::vector<float> tmp;
            while (m_running && !g_should_exit) {
                m_audio->get(m_chunk_ms, tmp);
                if (!tmp.empty()) {
                    m_buffer.push(tmp);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void get(int ms, std::vector<float>& audio) {
        size_t samples_needed = (static_cast<size_t>(SENSE_VOICE_SAMPLE_RATE) * ms) / 1000;
        m_buffer.read(samples_needed, audio, 20);
    }

    void clear() {
        m_buffer.clear();
    }

    void set_idle(bool idle) {
        m_buffer.set_overwrite_oldest(idle);
    }

private:
    int m_chunk_ms;
    std::unique_ptr<audio_async> m_audio;
    std::atomic<bool> m_running;
    std::thread m_thread;
    RingBuffer<float> m_buffer;
};

// ==========================================
// 指令解析（支持 START/STOP + 来源）
// ==========================================
enum class CommandType {
    Start,
    Stop,
    Exit,
    Unknown,
};

struct ParsedCommand {
    CommandType type = CommandType::Unknown;
    SourceKind source = SourceKind::None;
    bool has_source = false;
};

static SourceKind parse_source_token(const std::string& token) {
    if (token == "PC") return SourceKind::PC;
    if (token == "PHONE") return SourceKind::Phone;
    return SourceKind::None;
}

static ParsedCommand parse_command(const std::string& line) {
    ParsedCommand cmd;
    if (line.empty()) return cmd;

    std::string first;
    std::string second;
    {
        size_t pos = line.find(' ');
        if (pos == std::string::npos) {
            first = line;
        } else {
            first = line.substr(0, pos);
            second = line.substr(pos + 1);
            while (!second.empty() && second[0] == ' ') second.erase(0, 1);
        }
    }

    if (first == "START") cmd.type = CommandType::Start;
    else if (first == "STOP") cmd.type = CommandType::Stop;
    else if (first == "EXIT") cmd.type = CommandType::Exit;
    else cmd.type = CommandType::Unknown;

    if (!second.empty()) {
        cmd.source = parse_source_token(second);
        cmd.has_source = (cmd.source != SourceKind::None);
    }

    return cmd;
}

// ==========================================
// 核心逻辑：音频工作线程
// ==========================================
void AudioWorker(sense_voice_stream_params params) {
    bool mic_available = true;
    audio_async audio(params.chunk_size << 2);
    if (!audio.init(params.capture_id, SENSE_VOICE_SAMPLE_RATE)) {
        // 【v0.9.2.5 修复】SDL 初始化失败时不再直接 return
        // 而是继续加载模型，输出 ENGINE_READY，避免 C# 端永远等待
        fprintf(stderr, "[ZianCore] [WARNING] SDL microphone init failed, no local audio input available\n");
        mic_available = false;
    }
    if (mic_available) audio.resume();

    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) return;

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    std::vector<float> pcmf32_audio; // 临时接收音频 (Chunk)
    std::vector<double> pcmf32;      // 累积音频用于推理 (Inference Buffer)
    pcmf32.reserve(32000 * 30);

    // 【新增】全量录音缓存，用于保存 debug.wav
    // 【修改】只在开启时预留空间，否则不分配
    std::vector<float> full_session_audio; 
    if(params.save_audio){
        full_session_audio.reserve(16000 * 60); // 预留一分钟
    }
    
    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    
    // 分段逻辑变量
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f; 

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        // A. 录音状态
        if (g_is_recording && mic_available) {
            audio.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                
                // 1. (已移除) 应用耳语增强
                // apply_whisper_enhancement(pcmf32_audio);

                // 2. 计算音量 (用于分段)
                current_chunk_max_amp = 0.0f;
                for(float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                // 3. 存入推理 Buffer
                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                
                // 4. 【新增】存入调试保存 Buffer (保存的是增强后的声音)
                // 【修改】只有开启开关时，才往全量 buffer 里塞数据
                if(params.save_audio){
                    full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                }
                
                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // B. 触发推理
        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        // 智能分段判断
        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f); 
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_final_flush || trigger_segmentation) {
            
            // 最终冲刷时的尾部处理
            if (is_final_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                audio.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    // apply_whisper_enhancement(pcmf32_audio); // (已移除) 记得尾部也要增强

                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    
                    // 【新增】调试 Buffer 也要补上这一块
                    // 【修改】同样加上判断
                    if(params.save_audio){
                        full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());                        
                    }
                                      
                    pcmf32_audio.clear();
                }
            }

            // 执行推理
            int process_len = (int)pcmf32.size(); 
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    if (trigger_segmentation) std::cout << "SEG: "; 
                    else std::cout << "RES: "; 
                    
                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl; 
                    
                    idenitified_floats = process_len;
                }
            }

            // 分段清理 (只清空推理 Buffer，不清空录音文件 Buffer)
            if (trigger_segmentation) {
                pcmf32.clear();
                idenitified_floats = 0;
            }

            // 最终结束清理
            if (is_final_flush) {
                // 【新增】保存录音文件到本地
                // 注意：这会保存从按下 START 到 STOP 的完整过程，包括所有分段
                // 【修改】只有开启开关时才写文件
                if(params.save_audio){
                    // Generate timestamped filename
                    std::time_t now = std::time(nullptr);
                    struct tm tstruct;
                    char buf[80];
                    localtime_s(&tstruct, &now);
                    std::strftime(buf, sizeof(buf), "_%Y-%m-%d_%H-%M-%S", &tstruct);

                    std::string final_path = params.audio_path;
                    size_t lastindex = final_path.find_last_of("."); 
                    if (lastindex == std::string::npos) {
                        final_path += buf; 
                        final_path += ".wav";
                    } else {
                        final_path.insert(lastindex, buf); 
                    }
                     write_wav_file(final_path, full_session_audio, SAMPLE_RATE);
                    full_session_audio.clear(); // 清空以备下次使用
                }

                g_needs_flush = false; 
                pcmf32.clear();
                idenitified_floats = 0;
                if (mic_available) audio.clear(); 
                std::cout << "[[STOPPED]]" << std::endl; 
            }
        }

        if (!g_is_recording && !g_needs_flush) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free_stream(ctx);
}

// ==========================================
// 【Zian Link】基于 stdin 管道音频的工作线程
// ==========================================
void AudioWorkerPipe(sense_voice_stream_params params, audio_pipe& pipe) {
    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        std::cerr << "[AudioWorkerPipe] 模型加载失败" << std::endl;
        return;
    }

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    std::vector<float> pcmf32_audio; // 临时接收音频 (Chunk)
    std::vector<double> pcmf32;      // 累积音频用于推理 (Inference Buffer)
    pcmf32.reserve(32000 * 30);

    std::vector<float> full_session_audio;
    if(params.save_audio){
        full_session_audio.reserve(16000 * 60);
    }
    
    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f;

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        // A. 录音状态 - 从 pipe 获取音频
        if (g_is_recording) {
            pipe.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                current_chunk_max_amp = 0.0f;
                for(float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                
                if(params.save_audio){
                    full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                }
                
                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // B. 触发推理
        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f);
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_final_flush || trigger_segmentation) {
            
            if (is_final_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                pipe.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    if(params.save_audio){
                        full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    }
                    pcmf32_audio.clear();
                }
            }

            int process_len = (int)pcmf32.size();
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    if (trigger_segmentation) std::cout << "SEG: ";
                    else std::cout << "RES: ";
                    
                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl;
                    
                    idenitified_floats = process_len;
                }
            }

            if (trigger_segmentation) {
                pcmf32.clear();
                idenitified_floats = 0;
            }

            if (is_final_flush) {
                if(params.save_audio){
                    std::time_t now = std::time(nullptr);
                    struct tm tstruct;
                    char buf[80];
                    localtime_s(&tstruct, &now);
                    std::strftime(buf, sizeof(buf), "_%Y-%m-%d_%H-%M-%S", &tstruct);

                    std::string final_path = params.audio_path;
                    size_t lastindex = final_path.find_last_of(".");
                    if (lastindex == std::string::npos) {
                        final_path += buf;
                        final_path += ".wav";
                    } else {
                        final_path.insert(lastindex, buf);
                    }
                    write_wav_file(final_path, full_session_audio, SAMPLE_RATE);
                    full_session_audio.clear();
                }

                g_needs_flush = false;
                pcmf32.clear();
                idenitified_floats = 0;
                pipe.clear();
                std::cout << "[[STOPPED]]" << std::endl;
            }
        }

        if (!g_is_recording && !g_needs_flush) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free_stream(ctx);
}

// ==========================================
// 【Zian Link】双输入：SDL + Socket（Selection + Preempt）
// ==========================================
void AudioWorkerDual(sense_voice_stream_params params, sdl_mic_source& mic, audio_socket& sock) {
    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        std::cerr << "[AudioWorkerDual] 模型加载失败" << std::endl;
        return;
    }

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    std::vector<float> pcmf32_audio;
    std::vector<double> pcmf32;
    pcmf32.reserve(32000 * 30);

    std::vector<float> full_session_audio;
    if (params.save_audio) {
        full_session_audio.reserve(16000 * 60);
    }

    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f;

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        SourceKind active = static_cast<SourceKind>(g_active_source.load());

        if (g_is_recording && active != SourceKind::None) {
            if (active == SourceKind::PC) {
                mic.get(params.chunk_size, pcmf32_audio);
            } else if (active == SourceKind::Phone) {
                sock.get(params.chunk_size, pcmf32_audio);
            }

            if (!pcmf32_audio.empty()) {
                current_chunk_max_amp = 0.0f;
                for (float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());

                if (params.save_audio) {
                    full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                }

                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_stop_flush = (!g_is_recording && g_needs_flush);
        bool is_preempt_flush = g_preempt_requested.load();

        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f);
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_stop_flush || is_preempt_flush || trigger_segmentation) {
            if (is_stop_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (active == SourceKind::PC) {
                    mic.get(params.chunk_size, pcmf32_audio);
                } else if (active == SourceKind::Phone) {
                    sock.get(params.chunk_size, pcmf32_audio);
                }

                if (!pcmf32_audio.empty()) {
                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    if (params.save_audio) {
                        full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    }
                    pcmf32_audio.clear();
                }
            }

            int process_len = (int)pcmf32.size();
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    if (trigger_segmentation) std::cout << "SEG: ";
                    else std::cout << "RES: ";

                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl;

                    idenitified_floats = process_len;
                }
            }

            if (trigger_segmentation) {
                pcmf32.clear();
                idenitified_floats = 0;
            }

            if (is_preempt_flush) {
                g_preempt_requested = false;
                pcmf32_audio.clear();
                pcmf32.clear();
                idenitified_floats = 0;
                current_chunk_max_amp = 0.0f;
                if (params.save_audio) {
                    full_session_audio.clear();
                }
            }

            if (is_stop_flush) {
                if (params.save_audio) {
                    std::time_t now = std::time(nullptr);
                    struct tm tstruct;
                    char buf[80];
                    localtime_s(&tstruct, &now);
                    std::strftime(buf, sizeof(buf), "_%Y-%m-%d_%H-%M-%S", &tstruct);

                    std::string final_path = params.audio_path;
                    size_t lastindex = final_path.find_last_of(".");
                    if (lastindex == std::string::npos) {
                        final_path += buf;
                        final_path += ".wav";
                    } else {
                        final_path.insert(lastindex, buf);
                    }
                    write_wav_file(final_path, full_session_audio, SAMPLE_RATE);
                    full_session_audio.clear();
                }

                g_needs_flush = false;
                pcmf32.clear();
                idenitified_floats = 0;
                if (active == SourceKind::PC) {
                    mic.clear();
                } else if (active == SourceKind::Phone) {
                    sock.clear();
                }
                std::cout << "[[STOPPED]]" << std::endl;
            }
        }

        if (!g_is_recording && !g_needs_flush && !g_preempt_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free_stream(ctx);
}

// ==========================================
// 【Zian Link】基于 socket 音频的工作线程
// ==========================================
void AudioWorkerSocket(sense_voice_stream_params params, audio_socket& sock) {
    struct sense_voice_context_params cparams = sense_voice_context_default_params();
    cparams.use_gpu = params.use_gpu;
    cparams.flash_attn = params.flash_attn;
    cparams.use_itn = params.use_itn;

    struct sense_voice_context *ctx = sense_voice_small_init_from_file_with_params(params.model.c_str(), cparams);
    if (!ctx) {
        std::cerr << "[AudioWorkerSocket] 模型加载失败" << std::endl;
        return;
    }

    ctx->language_id = sense_voice_lang_id(params.language.c_str());
    if (ctx->language_id == -1) ctx->language_id = sense_voice_lang_id("auto");

    std::vector<float> pcmf32_audio;
    std::vector<double> pcmf32;
    pcmf32.reserve(32000 * 30);

    std::vector<float> full_session_audio;
    if(params.save_audio){
        full_session_audio.reserve(16000 * 60);
    }

    sense_voice_full_params wparams = sense_voice_full_default_params(SENSE_VOICE_SAMPLING_GREEDY);
    wparams.language = params.language.c_str();
    wparams.n_threads = params.n_threads;

    int idenitified_floats = 0;
    const int SAMPLE_RATE = SENSE_VOICE_SAMPLE_RATE;
    float current_chunk_max_amp = 0.0f;

    std::cout << "[[ENGINE_READY]]" << std::endl;

    while (!g_should_exit) {
        if (g_is_recording) {
            sock.get(params.chunk_size, pcmf32_audio);
            if (!pcmf32_audio.empty()) {
                current_chunk_max_amp = 0.0f;
                for(float f : pcmf32_audio) current_chunk_max_amp = std::max(current_chunk_max_amp, std::abs(f));

                pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());

                if(params.save_audio){
                    full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                }

                pcmf32_audio.clear();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        const int STEP_SAMPLES = (800 * SAMPLE_RATE) / 1000;
        bool should_inference = (g_is_recording && (pcmf32.size() > idenitified_floats + STEP_SAMPLES));
        bool is_final_flush = (!g_is_recording && g_needs_flush);

        bool trigger_segmentation = false;
        if (g_is_recording && pcmf32.size() > 0) {
            double duration_sec = (double)pcmf32.size() / SAMPLE_RATE;
            bool is_quiet = (current_chunk_max_amp < 0.05f);
            if (duration_sec > 15.0 && is_quiet) trigger_segmentation = true;
            else if (duration_sec > 28.0) trigger_segmentation = true;
        }

        if (should_inference || is_final_flush || trigger_segmentation) {
            if (is_final_flush) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                sock.get(params.chunk_size, pcmf32_audio);
                if (!pcmf32_audio.empty()) {
                    pcmf32.insert(pcmf32.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    if(params.save_audio){
                        full_session_audio.insert(full_session_audio.end(), pcmf32_audio.begin(), pcmf32_audio.end());
                    }
                    pcmf32_audio.clear();
                }
            }

            int process_len = (int)pcmf32.size();
            if (process_len > 0) {
                if (sense_voice_full_parallel(ctx, wparams, pcmf32, process_len, params.n_processors) == 0) {
                    if (trigger_segmentation) std::cout << "SEG: ";
                    else std::cout << "RES: ";

                    sense_voice_print_output(ctx, params.use_prefix, params.use_itn, true);
                    std::cout << std::endl;

                    idenitified_floats = process_len;
                }
            }

            if (trigger_segmentation) {
                pcmf32.clear();
                idenitified_floats = 0;
            }

            if (is_final_flush) {
                if(params.save_audio){
                    std::time_t now = std::time(nullptr);
                    struct tm tstruct;
                    char buf[80];
                    localtime_s(&tstruct, &now);
                    std::strftime(buf, sizeof(buf), "_%Y-%m-%d_%H-%M-%S", &tstruct);

                    std::string final_path = params.audio_path;
                    size_t lastindex = final_path.find_last_of(".");
                    if (lastindex == std::string::npos) {
                        final_path += buf;
                        final_path += ".wav";
                    } else {
                        final_path.insert(lastindex, buf);
                    }
                    write_wav_file(final_path, full_session_audio, SAMPLE_RATE);
                    full_session_audio.clear();
                }

                g_needs_flush = false;
                pcmf32.clear();
                idenitified_floats = 0;
                sock.clear();
                std::cout << "[[STOPPED]]" << std::endl;
            }
        }

        if (!g_is_recording && !g_needs_flush) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    sense_voice_free_stream(ctx);
}

int run_input_mode(int argc, char **argv) {
    std::setvbuf(stdout, NULL, _IONBF, 0);
    sense_voice_stream_params params;
    if (!get_stream_params(argc, argv, params)) return 1;

    // 【Zian Link】双输入模式（SDL + Socket）
    if (params.dual_audio) {
        std::cerr << "[ZianCore] 启用双输入模式 (SDL + Socket), 端口: " << params.socket_port << std::endl;

        audio_socket sock_audio(SENSE_VOICE_SAMPLE_RATE);
        if (!sock_audio.init(params.socket_port)) {
            std::cerr << "[ZianCore] audio_socket 初始化失败" << std::endl;
            return 1;
        }

        sdl_mic_source mic_audio(params.chunk_size);
        bool mic_available = mic_audio.init(params.capture_id, SENSE_VOICE_SAMPLE_RATE);

        if (!mic_available) {
            // SDL 麦克风不可用（无音频采集设备），降级为仅 Socket 模式
            std::cerr << "[ZianCore] [WARNING] SDL 麦克风不可用，降级为仅 Socket 模式" << std::endl;

            std::thread worker([&params, &sock_audio]() {
                AudioWorkerSocket(params, sock_audio);
            });

            std::string line;
            while (std::getline(std::cin, line)) {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                    line.pop_back();
                }

                ParsedCommand cmd = parse_command(line);
                if (cmd.type == CommandType::Exit) {
                    g_should_exit = true;
                    break;
                }

                if (cmd.type == CommandType::Start) {
                    g_active_source = static_cast<int>(SourceKind::Phone);
                    g_is_recording = true;
                    g_needs_flush = false;
                } else if (cmd.type == CommandType::Stop) {
                    g_is_recording = false;
                    g_needs_flush = true;
                }
            }

            if (worker.joinable()) worker.join();
            sock_audio.stop();
            return 0;
        }

        // SDL 麦克风可用，正常双输入模式
        mic_audio.set_idle(true);
        sock_audio.set_idle(true);

        mic_audio.start();

        std::thread worker([&params, &mic_audio, &sock_audio]() {
            AudioWorkerDual(params, mic_audio, sock_audio);
        });

        std::string line;
        while (std::getline(std::cin, line)) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }

            ParsedCommand cmd = parse_command(line);
            if (cmd.type == CommandType::Exit) {
                g_should_exit = true;
                break;
            }

            if (cmd.type == CommandType::Start) {
                SourceKind src = cmd.has_source ? cmd.source : SourceKind::PC;
                if (src == SourceKind::None) src = SourceKind::PC;

                SourceKind current = static_cast<SourceKind>(g_active_source.load());
                if (g_is_recording && current != SourceKind::None && current != src) {
                    g_preempt_requested = true;
                }

                g_active_source = static_cast<int>(src);
                if (src == SourceKind::PC) {
                    mic_audio.set_idle(false);
                    sock_audio.set_idle(true);
                    mic_audio.clear();
                } else if (src == SourceKind::Phone) {
                    mic_audio.set_idle(true);
                    sock_audio.set_idle(false);
                    sock_audio.clear();
                }

                g_is_recording = true;
                g_needs_flush = false;
            } else if (cmd.type == CommandType::Stop) {
                SourceKind current = static_cast<SourceKind>(g_active_source.load());
                SourceKind src = cmd.has_source ? cmd.source : current;

                if (src != SourceKind::None && src == current && g_is_recording) {
                    g_is_recording = false;
                    g_needs_flush = true;
                }
            }
        }

        if (worker.joinable()) worker.join();
        mic_audio.stop();
        sock_audio.stop();
        return 0;
    }

    // 【Zian Link】socket 音频模式（手机麦克风）
    if (params.socket_audio) {
        std::cerr << "[ZianCore] 启用 socket 音频输入模式, 端口: " << params.socket_port << std::endl;

        audio_socket sock_audio(SENSE_VOICE_SAMPLE_RATE);
        if (!sock_audio.init(params.socket_port)) {
            std::cerr << "[ZianCore] audio_socket 初始化失败" << std::endl;
            return 1;
        }

        std::thread worker([&params, &sock_audio]() {
            AudioWorkerSocket(params, sock_audio);
        });

        std::string line;
        while (std::getline(std::cin, line)) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }

            ParsedCommand cmd = parse_command(line);
            if (cmd.type == CommandType::Exit) {
                g_should_exit = true;
                break;
            }

            if (cmd.type == CommandType::Start) {
                SourceKind src = cmd.has_source ? cmd.source : SourceKind::Phone;
                if (src == SourceKind::Phone) {
                    g_active_source = static_cast<int>(SourceKind::Phone);
                    g_is_recording = true;
                    g_needs_flush = false;
                }
            } else if (cmd.type == CommandType::Stop) {
                SourceKind src = cmd.has_source ? cmd.source : SourceKind::Phone;
                if (src == SourceKind::Phone) {
                    g_is_recording = false;
                    g_needs_flush = true;
                }
            }
        }

        if (worker.joinable()) worker.join();
        return 0;
    }

    // 【Zian Link】stdin 音频模式
    if (params.stdin_audio) {
        std::cerr << "[ZianCore] 启用 stdin 音频输入模式 (帧协议)" << std::endl;
        
        // 初始化 pipe 音频源 - 它会启动自己的线程读取 stdin
        audio_pipe pipe_audio(SENSE_VOICE_SAMPLE_RATE);
        if (!pipe_audio.init()) {
            std::cerr << "[ZianCore] audio_pipe 初始化失败" << std::endl;
            return 1;
        }
        
        // 启动处理线程（使用 pipe_audio）
        std::thread worker([&params, &pipe_audio]() {
            AudioWorkerPipe(params, pipe_audio);
        });
        
        // audio_pipe 的 reader_thread 会处理 stdin，主线程等待退出
        while (!g_should_exit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (worker.joinable()) worker.join();
        return 0;
    }

    // 传统模式：文本指令 + 本地麦克风
    std::thread worker(AudioWorker, params);

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line == "START") {
            g_is_recording = true;
            g_needs_flush = false;
        } 
        else if (line == "STOP") {
            g_is_recording = false;
            g_needs_flush = true;
        }
        else if (line == "EXIT") {
            g_should_exit = true;
            break;
        }
    }

    if (worker.joinable()) worker.join();
    return 0;
}

// [新增] 打印音频设备列表
void print_audio_devices() {
    // 初始化音频子系统
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return;
    }

    int nDevices = SDL_GetNumAudioDevices(SDL_TRUE); // SDL_TRUE = capture devices
    std::cout << "Found " << nDevices << " capture devices:" << std::endl;
    for (int i = 0; i < nDevices; ++i) {
        const char* name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        std::cout << i << ": " << (name ? name : "Unknown") << std::endl;
    }
    
    SDL_Quit();
}