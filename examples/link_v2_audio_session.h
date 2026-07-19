#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

struct link_v2_audio_key_material {
    std::array<uint8_t, 16> session_id;
    std::array<uint8_t, 32> audio_key;
    std::array<uint8_t, 4> nonce_prefix;
};

class link_v2_audio_session_registry {
public:
    ~link_v2_audio_session_registry();

    bool register_session(const std::string& session_id_hex,
                          const std::string& audio_key_hex,
                          const std::string& nonce_prefix_hex);
    bool remove_session(const std::string& session_id_hex);
    bool find_session(const std::array<uint8_t, 16>& session_id,
                      link_v2_audio_key_material& material) const;
    void clear();
    size_t size() const;

private:
    static bool decode_hex(const std::string& text, uint8_t* output, size_t length);
    static std::string encode_hex(const uint8_t* data, size_t length);
    static void clear_material(link_v2_audio_key_material& material);

    mutable std::mutex m_mutex;
    std::map<std::string, link_v2_audio_key_material> m_sessions;
};

enum class link_v2_audio_control_result {
    NotHandled,
    Accepted,
    Rejected,
};

link_v2_audio_control_result handle_link_v2_audio_control(
    const std::string& line,
    link_v2_audio_session_registry& registry);
