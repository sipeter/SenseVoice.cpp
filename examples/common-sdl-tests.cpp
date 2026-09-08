#define SDL_MAIN_HANDLED
#include "common-sdl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic<bool> block_close { false };
std::atomic<bool> close_entered { false };
bool fail_open = false;
SDL_AudioStatus device_status = SDL_AUDIO_PLAYING;
SDL_AudioDeviceID next_id = 2;

void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

extern "C" {
void SDLCALL SDL_LogSetPriority(int, SDL_LogPriority) {}
void SDLCALL SDL_LogError(int, const char *, ...) {}
int SDLCALL SDL_Init(Uint32) { return 0; }
const char * SDLCALL SDL_GetError() { return "injected open failure"; }
SDL_bool SDLCALL SDL_SetHintWithPriority(const char *, const char *, SDL_HintPriority) { return SDL_TRUE; }
int SDLCALL SDL_GetNumAudioDevices(int) { return 1; }
const char * SDLCALL SDL_GetAudioDeviceName(int, int) { return "test microphone"; }
int SDLCALL SDL_GetDefaultAudioInfo(char ** name, SDL_AudioSpec *, int) { *name = nullptr; return -1; }
void SDLCALL SDL_free(void * p) { std::free(p); }
void * SDLCALL SDL_memset(void * p, int c, size_t n) { return std::memset(p, c, n); }
SDL_AudioDeviceID SDLCALL SDL_OpenAudioDevice(const char *, int, const SDL_AudioSpec * want, SDL_AudioSpec * got, int) {
    if (fail_open) return 0;
    *got = *want;
    return next_id++;
}
void SDLCALL SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
SDL_AudioStatus SDLCALL SDL_GetAudioDeviceStatus(SDL_AudioDeviceID) { return device_status; }
int SDLCALL SDL_PollEvent(SDL_Event *) { return 0; }
void SDLCALL SDL_CloseAudioDevice(SDL_AudioDeviceID) {
    close_entered = true;
    while (block_close) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}

struct audio_async_recovery_tests {
    static void run() {
        audio_async audio(5000);
        audio.m_requested_sample_rate = 16000;
        audio.m_running = true;
        require(audio.open_capture_device(), "first open failed");
        require(audio.m_microphone_state == audio_async::microphone_starting, "open must not report online");
        require(audio.m_recovery_opened_at_ms != 0, "first open must await capture confirmation");
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_starting, "no callback must not report online");

        float silence[1024] = {};
        uint8_t * samples = reinterpret_cast<uint8_t *>(silence);
        const uint64_t old_generation = audio.m_active_device_generation.load();
        audio.callback(nullptr, sizeof(silence), old_generation);
        audio.callback(samples, 0, old_generation);
        audio.callback(samples, 1, old_generation);
        require(!audio.m_recovery_callback_confirmed, "invalid audio must not confirm capture");
        audio.callback(samples, sizeof(silence), old_generation);
        require(audio.m_recovery_callback_confirmed, "valid silence must confirm first capture");
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_online, "fresh Core must report MIC_ONLINE");
        require(!audio.m_recovery_callback_confirmed, "confirmation must be consumed");
        audio.confirm_microphone_recovered();

        audio.close_capture_device("test disconnect");
        require(audio.m_microphone_state == audio_async::microphone_recovering, "disconnect must report offline");
        fail_open = true;
        require(!audio.open_capture_device(), "injected open failure was ignored");
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_recovering, "open failure must stay offline");
        fail_open = false;
        require(audio.open_capture_device(), "reopen failed");
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_recovering, "reopen without callback must stay offline");
        audio.callback(samples, sizeof(silence), old_generation);
        require(!audio.m_recovery_callback_confirmed, "late old callback must not confirm the reopened device");
        audio.callback(samples, sizeof(silence), audio.m_active_device_generation.load());
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_online, "current callback must restore online");

        close_entered = false;
        block_close = true;
        std::thread closing([&]() { audio.close_capture_device("test blocked close"); });
        const int64_t deadline = now_ms() + 3000;
        while (!close_entered && now_ms() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool entered = close_entered.load();
        const bool offline_before_close_returns = audio.m_microphone_state == audio_async::microphone_recovering;
        const bool capture_revoked = audio.m_dev_id_in == 0 && audio.m_active_device_generation == 0;
        block_close = false;
        closing.join();
        require(entered, "close fault injection timed out");
        require(offline_before_close_returns && capture_revoked, "offline must be emitted before a blocking SDL close");

        require(audio.open_capture_device(), "second reopen failed");
        audio.callback(samples, sizeof(silence), audio.m_active_device_generation.load());
        audio.confirm_microphone_recovered();
        const SDL_AudioDeviceID healthy_device = audio.m_dev_id_in.load();
        audio.m_last_capture_callback_ms = now_ms() - 2100;
        audio.reconnect_if_needed();
        require(audio.m_dev_id_in == healthy_device, "two-second callback delay must not disconnect");
        audio.m_last_capture_callback_ms = now_ms() - 5100;
        audio.reconnect_if_needed();
        require(audio.m_dev_id_in != healthy_device, "five-second stall must reopen capture");
        require(audio.m_microphone_state == audio_async::microphone_recovering, "stalled capture must await a new callback");

        // Do not briefly report online for a device that has already stopped again.
        audio.callback(samples, sizeof(silence), audio.m_active_device_generation.load());
        device_status = SDL_AUDIO_STOPPED;
        audio.reconnect_if_needed();
        require(audio.m_microphone_state == audio_async::microphone_recovering, "stopped device must not confirm recovery");
        device_status = SDL_AUDIO_PLAYING;
        audio.m_last_reconnect_attempt = std::chrono::steady_clock::time_point();
        audio.reconnect_if_needed();
        audio.callback(samples, sizeof(silence), audio.m_active_device_generation.load());
        audio.confirm_microphone_recovered();
        require(audio.m_microphone_state == audio_async::microphone_online, "subsequent recovery failed");
        audio.close_capture_device("shutdown");
        require(audio.m_microphone_state == audio_async::microphone_online, "shutdown must not emit offline");

        audio_async initially_unavailable(5000);
        initially_unavailable.m_requested_sample_rate = 16000;
        initially_unavailable.m_running = true;
        fail_open = true;
        require(!initially_unavailable.open_capture_device(), "initial open failure was ignored");
        require(initially_unavailable.m_microphone_state == audio_async::microphone_recovering,
                "initial open failure must report offline from the starting state");
        fail_open = false;
    }
};

int main() {
    try {
        audio_async_recovery_tests::run();
        std::cout << "PASS: microphone recovery, callback generations, stall threshold and blocked close" << std::endl;
        return 0;
    } catch (const std::exception & error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
