#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "ring_buffer.h"

class audio_socket {
public:
    explicit audio_socket(int sample_rate);
    ~audio_socket();

    bool init(int port);
    void stop();

    void get(int ms, std::vector<float>& audio);
    void clear();
    void set_idle(bool idle);

private:
    void server_thread(int port);
    void handle_client(int client_socket);
    void push_pcm16(const int16_t* data, size_t sample_count);

    int m_sample_rate;
    std::atomic<bool> m_running;
    std::thread m_thread;
    RingBuffer<float> m_buffer;
};
