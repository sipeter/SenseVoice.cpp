#include "link_v2_audio_session.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    link_v2_audio_session_registry registry;
    const std::string session = "00112233445566778899aabbccddeeff";
    const std::string key =
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f";
    const std::string prefix = "a1b2c3d4";

    require(handle_link_v2_audio_control(
        "LINK_V2_AUDIO_REGISTER " + session + " " + key + " " + prefix,
        registry) == link_v2_audio_control_result::Accepted, "registration failed");
    require(registry.size() == 1, "unexpected registry size after registration");

    std::array<uint8_t, 16> session_bytes{{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}};
    link_v2_audio_key_material material{};
    require(registry.find_session(session_bytes, material), "registered session not found");
    require(material.audio_key[0] == 0x00 && material.audio_key[31] == 0x1f,
            "audio key mismatch");
    require(material.nonce_prefix[0] == 0xa1 && material.nonce_prefix[3] == 0xd4,
            "nonce prefix mismatch");

    require(handle_link_v2_audio_control(
        "LINK_V2_AUDIO_REGISTER " + session + " bad " + prefix,
        registry) == link_v2_audio_control_result::Rejected, "invalid registration accepted");
    require(registry.size() == 1, "invalid registration changed registry");
    require(registry.find_session(session_bytes, material), "invalid registration removed session");
    require(material.audio_key[31] == 0x1f, "invalid registration replaced session key");
    require(handle_link_v2_audio_control(
        "LINK_V2_AUDIO_REMOVE " + session,
        registry) == link_v2_audio_control_result::Accepted, "removal failed");
    require(registry.size() == 0, "unexpected registry size after removal");
    require(!registry.find_session(session_bytes, material), "removed session still found");

    require(handle_link_v2_audio_control("START PHONE", registry) ==
        link_v2_audio_control_result::NotHandled, "legacy command was consumed");
    require(handle_link_v2_audio_control("LINK_V2_AUDIO_CLEAR extra", registry) ==
        link_v2_audio_control_result::Rejected, "malformed clear accepted");

    std::cout << "link_v2_audio_session_tests passed" << std::endl;
    return 0;
}
