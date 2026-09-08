#pragma once

#include <SDL.h>
#include <SDL_audio.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

//
// SDL Audio capture
//

class audio_async {
public:
    audio_async(int len_ms);
    ~audio_async();

    bool init(int capture_id, int sample_rate);

    // start capturing audio via the provided SDL callback
    // keep last len_ms seconds of audio in a circular buffer
    bool resume();
    bool pause();
    bool clear();

    // get audio data from the circular buffer
    void get(int ms, std::vector<float> & audio);

private:
    friend struct audio_async_recovery_tests;

    bool open_capture_device();
    void close_capture_device(const char * reason);
    void pump_device_events();
    void reconnect_if_needed();
    void device_monitor_loop();
    bool capture_device_is_present() const;
    void begin_microphone_recovery(const char * reason);
    void confirm_microphone_recovered();
    void callback(uint8_t * stream, int len, uint64_t device_generation);

    enum microphone_state {
        microphone_starting = 0,
        microphone_online = 1,
        microphone_recovering = 2,
    };

    struct callback_context {
        audio_async * owner;
        uint64_t device_generation;
    };

    std::atomic<SDL_AudioDeviceID> m_dev_id_in { 0 };
    std::atomic<uint64_t> m_active_device_generation { 0 };
    uint64_t m_next_device_generation = 0;
    std::vector<std::unique_ptr<callback_context>> m_callback_contexts;
    int m_capture_id = -1;
    int m_requested_sample_rate = 0;
    std::string m_capture_name;
    std::chrono::steady_clock::time_point m_last_reconnect_attempt;
    std::atomic<int64_t> m_last_capture_callback_ms { 0 };
    std::atomic<int64_t> m_recovery_opened_at_ms { 0 };
    std::atomic<int> m_microphone_state { microphone_starting };
    std::atomic_bool m_recovery_callback_confirmed { false };
    bool m_has_opened_once = false;
    bool m_reconnect_failure_logged = false;
    std::atomic_bool m_device_monitor_stop { false };
    std::thread m_device_monitor;
    std::mutex m_device_mutex;

    int m_len_ms = 0;
    int m_sample_rate = 0;

    std::atomic_bool m_running;
    std::mutex       m_mutex;

    std::vector<float> m_audio;
    size_t             m_audio_pos = 0;
    size_t             m_audio_len = 0;  // 调整定义：改为音频的有效长度
};

// Return false if need to quit
bool sdl_poll_events();
