#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>

//
// Pipe Audio Input (for Zian Link)
// 从 stdin 读取 PCM 16-bit 音频数据
//

class audio_pipe {
public:
    audio_pipe(int sample_rate = 16000);
    ~audio_pipe();

    // 初始化（创建读取线程）
    bool init();
    
    // 外部推送 PCM 数据（用于 C# 通过 stdin 发送）
    void push_pcm16(const int16_t* data, size_t sample_count);
    
    // 获取音频数据（与 audio_async 接口兼容）
    void get(int ms, std::vector<float>& audio);
    
    // 清空缓冲区
    void clear();
    
    // 状态
    bool is_running() const { return m_running; }

private:
    // 从 stdin 读取线程
    void reader_thread();
    
    int m_sample_rate;
    std::atomic_bool m_running;
    std::mutex m_mutex;
    
    // 音频缓冲区（float 格式，范围 [-1, 1]）
    std::vector<float> m_buffer;
    
    std::thread m_reader;
};
