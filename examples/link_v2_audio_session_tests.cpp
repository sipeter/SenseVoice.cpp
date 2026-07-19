#include "link_v2_audio_session.h"
#include "link_v2_audio_frame.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> hex(const std::string& text) {
    std::vector<uint8_t> output;
    output.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        output.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return output;
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

    link_v2_audio_session_registry frame_registry;
    require(frame_registry.register_session(session,
        "e9cc0d0fc2aa95ab4bfc5ab164681a921dd91f6fb02f3d0afd69fae3c4db460c",
        "ece763a9"), "frame session registration failed");
    const std::string frozen_frame =
        "5a4c3241020000112233445566778899aabbccddeeff000000000000000100000008"
        "56fbc55b8576523d83a8a4e03e9ce80d49d745aef2d7439c";
    std::vector<uint8_t> plaintext;
    require(decode_link_v2_audio_frame(hex(frozen_frame), frame_registry, plaintext) ==
        link_v2_audio_frame_result::Accepted, "frozen ZL2A frame rejected");
    require(plaintext == hex("01000200ff7f0080"), "frozen ZL2A plaintext mismatch");
    require(decode_link_v2_audio_frame(hex(frozen_frame), frame_registry, plaintext) ==
        link_v2_audio_frame_result::Replay, "ZL2A replay accepted");

    require(frame_registry.remove_session(session), "frame session removal failed");
    require(frame_registry.register_session(session,
        "e9cc0d0fc2aa95ab4bfc5ab164681a921dd91f6fb02f3d0afd69fae3c4db460c",
        "ece763a9"), "frame session re-registration failed");
    auto tampered = hex(frozen_frame);
    tampered.back() ^= 1;
    require(decode_link_v2_audio_frame(tampered, frame_registry, plaintext) ==
        link_v2_audio_frame_result::AuthenticationFailed, "tampered ZL2A frame accepted");
    require(decode_link_v2_audio_frame(hex(frozen_frame), frame_registry, plaintext) ==
        link_v2_audio_frame_result::Accepted, "tag failure consumed ZL2A sequence");

    auto odd = hex(frozen_frame);
    odd[33] = 7;
    odd.erase(odd.begin() + 34 + 7);
    require(decode_link_v2_audio_frame(odd, frame_registry, plaintext) ==
        link_v2_audio_frame_result::InvalidFrame, "odd PCM length accepted");

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
