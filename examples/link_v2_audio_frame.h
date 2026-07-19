#pragma once

#include <cstdint>
#include <vector>

#include "link_v2_audio_session.h"

enum class link_v2_audio_frame_result {
    Accepted,
    InvalidFrame,
    UnknownSession,
    Replay,
    AuthenticationFailed,
};

link_v2_audio_frame_result decode_link_v2_audio_frame(
    const std::vector<uint8_t>& frame,
    link_v2_audio_session_registry& registry,
    std::vector<uint8_t>& plaintext);
