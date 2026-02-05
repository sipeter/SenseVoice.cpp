#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class audio_socket {
public:
    explicit audio_socket(int sample_rate);
    ~audio_socket();

    bool init(int port);
    void stop();

    void get(int ms, std::vector<float>& audio);
    void clear();

private:
    void server_thread(int port);
    void handle_client(int client_socket);
    void push_pcm16(const int16_t* data, size_t sample_count);

    int m_sample_rate;
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::mutex m_mutex;
    std::vector<float> m_buffer;
};
