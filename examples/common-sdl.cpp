#include "common-sdl.h"

#include <cstring>
#include <cstdio>

audio_async::audio_async(int len_ms) {
    m_len_ms = len_ms;

    m_running = false;
}

audio_async::~audio_async() {
    m_device_monitor_stop = true;
    if (m_device_monitor.joinable()) {
        m_device_monitor.join();
    }
    close_capture_device("shutdown");
}

bool audio_async::init(int capture_id, int sample_rate) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_RESAMPLING_MODE, "medium", SDL_HINT_OVERRIDE);

    m_capture_id = capture_id;
    m_requested_sample_rate = sample_rate;
    if (capture_id >= 0) {
        const char * capture_name = SDL_GetAudioDeviceName(capture_id, SDL_TRUE);
        if (capture_name != nullptr) {
            m_capture_name = capture_name;
        }
    } else {
        char * default_name = nullptr;
        if (SDL_GetDefaultAudioInfo(&default_name, nullptr, SDL_TRUE) == 0 && default_name != nullptr) {
            m_capture_name = default_name;
            SDL_free(default_name);
        }
    }

    {
        int nDevices = SDL_GetNumAudioDevices(SDL_TRUE);
        fprintf(stderr, "%s: found %d capture devices:\n", __func__, nDevices);
        for (int i = 0; i < nDevices; i++) {
            fprintf(stderr, "%s:    - Capture device #%d: '%s'\n", __func__, i, SDL_GetAudioDeviceName(i, SDL_TRUE));
        }
    }

    // Device open/close can temporarily block in the Windows audio backend
    // while a USB device is being removed or enumerated. Keep that work off
    // the inference loop so STOP can always flush and emit [[STOPPED]].
    m_device_monitor_stop = false;
    m_device_monitor = std::thread(&audio_async::device_monitor_loop, this);
    return true;
}

bool audio_async::open_capture_device() {
    std::lock_guard<std::mutex> device_lock(m_device_mutex);
    if (m_dev_id_in.load() != 0) return true;

    SDL_AudioSpec capture_spec_requested;
    SDL_AudioSpec capture_spec_obtained;

    SDL_zero(capture_spec_requested);
    SDL_zero(capture_spec_obtained);

    capture_spec_requested.freq     = m_requested_sample_rate;
    capture_spec_requested.format   = AUDIO_F32;
    capture_spec_requested.channels = 1;
    capture_spec_requested.samples  = 1024;
    capture_spec_requested.callback = [](void * userdata, uint8_t * stream, int len) {
        audio_async * audio = (audio_async *) userdata;
        audio->callback(stream, len);
    };
    capture_spec_requested.userdata = this;

    const char * device_name = m_capture_name.empty() ? nullptr : m_capture_name.c_str();
    const bool log_attempt = !m_has_opened_once || !m_reconnect_failure_logged;
    if (m_capture_id >= 0) {
        if (device_name == nullptr) {
            emit_offline_signal();
            if (log_attempt) {
                fprintf(stderr, "%s: configured capture device %d is unavailable\n", __func__, m_capture_id);
                m_reconnect_failure_logged = true;
            }
            return false;
        }
        if (log_attempt) {
            fprintf(stderr, "%s: attempt to open capture device %d : '%s' ...\n",
                    __func__, m_capture_id, device_name);
        }
    } else {
        if (log_attempt) {
            fprintf(stderr, "%s: attempt to open default capture device ...\n", __func__);
        }
    }

    SDL_AudioDeviceID opened = SDL_OpenAudioDevice(
            device_name,
            SDL_TRUE,
            &capture_spec_requested,
            &capture_spec_obtained,
            0);
    if (!opened) {
        emit_offline_signal();
        if (log_attempt) {
            fprintf(stderr, "%s: couldn't open an audio device for capture: %s!\n", __func__, SDL_GetError());
            m_reconnect_failure_logged = true;
        }
        return false;
    }

    m_sample_rate = capture_spec_obtained.freq;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audio.assign((m_sample_rate*m_len_ms)/1000, 0.0f);
        m_audio_pos = 0;
        m_audio_len = 0;
    }
    m_dev_id_in = opened;
    m_last_capture_callback_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    fprintf(stderr, "%s: obtained spec for input device (SDL Id = %d):\n", __func__, opened);
    fprintf(stderr, "%s:     - sample rate:       %d\n",                   __func__, capture_spec_obtained.freq);
    fprintf(stderr, "%s:     - format:            %d (required: %d)\n",    __func__, capture_spec_obtained.format,
            capture_spec_requested.format);
    fprintf(stderr, "%s:     - channels:          %d (required: %d)\n",    __func__, capture_spec_obtained.channels,
            capture_spec_requested.channels);
    fprintf(stderr, "%s:     - samples per frame: %d\n",                   __func__, capture_spec_obtained.samples);

    if (m_running) {
        SDL_PauseAudioDevice(opened, 0);
    }
    if (m_has_opened_once) {
        fprintf(stderr, "[ZianCore] audio capture device reconnected: '%s' (SDL Id = %d)\n",
                device_name == nullptr ? "default" : device_name,
                opened);
    }
    m_has_opened_once = true;
    m_reconnect_failure_logged = false;
    m_offline_signal_sent = false;
    return true;
}

void audio_async::close_capture_device(const char * reason) {
    std::lock_guard<std::mutex> device_lock(m_device_mutex);
    const SDL_AudioDeviceID closing = m_dev_id_in.exchange(0);
    if (!closing) return;
    const bool was_running = m_running.exchange(false);
    SDL_CloseAudioDevice(closing);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_audio_pos = 0;
        m_audio_len = 0;
    }
    if (was_running) {
        m_running = true;
    }
    if (reason == nullptr || std::strcmp(reason, "shutdown") != 0) {
        emit_offline_signal();
    }
    m_reconnect_failure_logged = false;
    fprintf(stderr, "[ZianCore] audio capture device closed (%s, SDL Id = %d)\n",
            reason == nullptr ? "unknown" : reason,
            closing);
}

void audio_async::emit_offline_signal() {
    if (m_offline_signal_sent) return;
    fprintf(stdout, "[[MIC_OFFLINE]]\n");
    fflush(stdout);
    m_offline_signal_sent = true;
}

bool audio_async::capture_device_is_present() const {
    if (m_capture_name.empty()) return true;

    const int count = SDL_GetNumAudioDevices(SDL_TRUE);
    if (count < 0) return true;
    for (int i = 0; i < count; ++i) {
        const char * name = SDL_GetAudioDeviceName(i, SDL_TRUE);
        if (name != nullptr && m_capture_name == name) return true;
    }
    return false;
}

void audio_async::pump_device_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_AUDIODEVICEREMOVED &&
            event.adevice.iscapture &&
            m_dev_id_in &&
            event.adevice.which == m_dev_id_in) {
            close_capture_device("device removed");
        } else if (event.type == SDL_AUDIODEVICEADDED && event.adevice.iscapture) {
            m_last_reconnect_attempt = std::chrono::steady_clock::time_point();
            m_reconnect_failure_logged = false;
        }
    }
}

void audio_async::reconnect_if_needed() {
    pump_device_events();

    const SDL_AudioDeviceID current = m_dev_id_in.load();
    if (current && m_running) {
        if (!capture_device_is_present()) {
            close_capture_device("device no longer enumerated");
        } else if (SDL_GetAudioDeviceStatus(current) == SDL_AUDIO_STOPPED) {
            close_capture_device("device stopped");
        } else {
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t callback_ms = m_last_capture_callback_ms.load();
            if (callback_ms != 0 && now_ms - callback_ms > 2000) {
                close_capture_device("capture callback stalled");
            }
        }
    }
    if (m_dev_id_in.load() || !m_running) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_last_reconnect_attempt.time_since_epoch().count() != 0 &&
        now - m_last_reconnect_attempt < std::chrono::milliseconds(500)) {
        return;
    }
    m_last_reconnect_attempt = now;
    open_capture_device();
}

void audio_async::device_monitor_loop() {
    while (!m_device_monitor_stop) {
        reconnect_if_needed();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool audio_async::resume() {
    if (m_running.exchange(true)) {
        fprintf(stderr, "%s: already running!\n", __func__);
        return false;
    }

    std::lock_guard<std::mutex> device_lock(m_device_mutex);
    const SDL_AudioDeviceID current = m_dev_id_in.load();
    if (current) {
        SDL_PauseAudioDevice(current, 0);
    } else {
        fprintf(stderr, "%s: no capture device yet; background reconnect is active\n", __func__);
    }

    return true;
}

bool audio_async::pause() {
    if (!m_running.exchange(false)) {
        fprintf(stderr, "%s: already paused!\n", __func__);
        return false;
    }

    std::lock_guard<std::mutex> device_lock(m_device_mutex);
    const SDL_AudioDeviceID current = m_dev_id_in.load();
    if (current) {
        SDL_PauseAudioDevice(current, 1);
    }

    return true;
}

bool audio_async::clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_audio_pos = 0;
        m_audio_len = 0;
    }

    return true;
}

// callback to be called by SDL
void audio_async::callback(uint8_t * stream, int len) {
    if (!m_running) {
        return;
    }

    m_last_capture_callback_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    size_t n_samples = len / sizeof(float);

    if (n_samples > m_audio.size()) {
        n_samples = m_audio.size();

        stream += (len - (n_samples * sizeof(float)));
    }

    // fprintf(stderr, "%s || samples: %zu, pos: %zu, audiolen: %zu, len: %d\n", __func__, n_samples, m_audio_pos, m_audio_len, len);

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_audio_pos + n_samples > m_audio.size()) {
            const size_t n0 = m_audio.size() - m_audio_pos;

            memcpy(&m_audio[m_audio_pos], stream, n0 * sizeof(float));
            memcpy(&m_audio[0], stream + n0 * sizeof(float), (n_samples - n0) * sizeof(float));

            m_audio_pos = (m_audio_pos + n_samples) % m_audio.size();
        } else {
            memcpy(&m_audio[m_audio_pos], stream, n_samples * sizeof(float));

            m_audio_pos = (m_audio_pos + n_samples) % m_audio.size();
        }
        m_audio_len = std::min(m_audio_len + n_samples, m_audio.size());
    }
}

void audio_async::get(int ms, std::vector<float> & result) {
    result.clear();
    if (!m_dev_id_in.load()) {
        return;
    }

    if (!m_running) {
        fprintf(stderr, "%s: not running!\n", __func__);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        result.resize(m_audio_len);
        int s0 = m_audio_pos - m_audio_len;
        // fprintf(stderr, "%s || pos: %zu, audiolen: %zu, s0: %d\n", __func__, m_audio_pos, m_audio_len, s0);
        if (s0 < 0) {
            s0 += m_audio.size();
            const size_t n0 = m_audio.size() - s0;
            // fprintf(stderr, "%s || s0: %d, n0: %zu\n", __func__, s0, n0);
            memcpy(result.data(), &m_audio[s0], n0 * sizeof(float));
            memcpy(&result[n0], &m_audio[0], (m_audio_len - n0) * sizeof(float));
        } else {
            memcpy(result.data(), &m_audio[s0], m_audio_len * sizeof(float));
        }
        m_audio_len = 0;
    }
}

bool sdl_poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                {
                    return false;
                }
            default:
                break;
        }
    }

    return true;
}
