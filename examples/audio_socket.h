#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "ring_buffer.h"
#include "link_v2_audio_session.h"

class audio_socket {
public:
    explicit audio_socket(int sample_rate);
    ~audio_socket();

    bool init(int port);
    void stop();

    void get(int ms, std::vector<float>& audio);
    void clear();
    void set_idle(bool idle);
    link_v2_audio_control_result handle_control(const std::string& line);

private:
    void server_thread(int port);
    void handle_client(int client_socket);
    void push_pcm16(const int16_t* data, size_t sample_count);

    int m_sample_rate;
    std::atomic<bool> m_running;
    std::thread m_thread;
    RingBuffer<float> m_buffer;
    link_v2_audio_session_registry m_v2_sessions;
};
