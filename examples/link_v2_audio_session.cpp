#include "link_v2_audio_session.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace {
const char* kRegister = "LINK_V2_AUDIO_REGISTER";
const char* kRemove = "LINK_V2_AUDIO_REMOVE";
const char* kClear = "LINK_V2_AUDIO_CLEAR";

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
}

link_v2_audio_session_registry::~link_v2_audio_session_registry() {
    clear();
}

bool link_v2_audio_session_registry::register_session(
    const std::string& session_id_hex,
    const std::string& audio_key_hex,
    const std::string& nonce_prefix_hex) {
    link_v2_audio_key_material material{};
    if (!decode_hex(session_id_hex, material.session_id.data(), material.session_id.size()) ||
        !decode_hex(audio_key_hex, material.audio_key.data(), material.audio_key.size()) ||
        !decode_hex(nonce_prefix_hex, material.nonce_prefix.data(), material.nonce_prefix.size())) {
        clear_material(material);
        return false;
    }

    const std::string key = encode_hex(material.session_id.data(), material.session_id.size());
    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_sessions.find(key);
    if (existing != m_sessions.end()) clear_material(existing->second);
    m_sessions[key] = material;
    clear_material(material);
    return true;
}

bool link_v2_audio_session_registry::remove_session(const std::string& session_id_hex) {
    std::array<uint8_t, 16> session_id{};
    if (!decode_hex(session_id_hex, session_id.data(), session_id.size())) return false;
    const std::string key = encode_hex(session_id.data(), session_id.size());
    std::fill(session_id.begin(), session_id.end(), static_cast<uint8_t>(0));

    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_sessions.find(key);
    if (existing == m_sessions.end()) return false;
    clear_material(existing->second);
    m_sessions.erase(existing);
    return true;
}

bool link_v2_audio_session_registry::find_session(
    const std::array<uint8_t, 16>& session_id,
    link_v2_audio_key_material& material) const {
    const std::string key = encode_hex(session_id.data(), session_id.size());
    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_sessions.find(key);
    if (existing == m_sessions.end()) return false;
    material = existing->second;
    return true;
}

bool link_v2_audio_session_registry::commit_sequence(
    const std::array<uint8_t, 16>& session_id, uint64_t sequence) {
    if (sequence == 0) return false;
    const std::string key = encode_hex(session_id.data(), session_id.size());
    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_sessions.find(key);
    if (existing == m_sessions.end() || sequence <= existing->second.last_sequence) return false;
    existing->second.last_sequence = sequence;
    return true;
}

void link_v2_audio_session_registry::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_sessions) clear_material(entry.second);
    m_sessions.clear();
}

size_t link_v2_audio_session_registry::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sessions.size();
}

bool link_v2_audio_session_registry::decode_hex(
    const std::string& text, uint8_t* output, size_t length) {
    if (text.size() != length * 2) return false;
    for (size_t i = 0; i < length; ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            std::fill(output, output + length, static_cast<uint8_t>(0));
            return false;
        }
        output[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

std::string link_v2_audio_session_registry::encode_hex(const uint8_t* data, size_t length) {
    static const char alphabet[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (size_t i = 0; i < length; ++i) {
        result[i * 2] = alphabet[data[i] >> 4];
        result[i * 2 + 1] = alphabet[data[i] & 0x0f];
    }
    return result;
}

void link_v2_audio_session_registry::clear_material(link_v2_audio_key_material& material) {
    std::fill(material.session_id.begin(), material.session_id.end(), static_cast<uint8_t>(0));
    std::fill(material.audio_key.begin(), material.audio_key.end(), static_cast<uint8_t>(0));
    std::fill(material.nonce_prefix.begin(), material.nonce_prefix.end(), static_cast<uint8_t>(0));
    material.last_sequence = 0;
}

link_v2_audio_control_result handle_link_v2_audio_control(
    const std::string& line,
    link_v2_audio_session_registry& registry) {
    std::istringstream input(line);
    std::vector<std::string> fields;
    std::string field;
    while (input >> field) fields.push_back(field);
    if (fields.empty()) return link_v2_audio_control_result::NotHandled;

    if (fields[0] == kRegister) {
        if (fields.size() != 4) return link_v2_audio_control_result::Rejected;
        return registry.register_session(fields[1], fields[2], fields[3])
            ? link_v2_audio_control_result::Accepted
            : link_v2_audio_control_result::Rejected;
    }
    if (fields[0] == kRemove) {
        if (fields.size() != 2) return link_v2_audio_control_result::Rejected;
        return registry.remove_session(fields[1])
            ? link_v2_audio_control_result::Accepted
            : link_v2_audio_control_result::Rejected;
    }
    if (fields[0] == kClear) {
        if (fields.size() != 1) return link_v2_audio_control_result::Rejected;
        registry.clear();
        return link_v2_audio_control_result::Accepted;
    }
    return link_v2_audio_control_result::NotHandled;
}
