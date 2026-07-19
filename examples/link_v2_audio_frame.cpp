#include "link_v2_audio_frame.h"

#include <algorithm>
#include <array>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace {
const size_t kHeaderSize = 34;
const size_t kTagSize = 16;
const uint32_t kMaximumPlaintext = 65536;

uint32_t read_u32be(const uint8_t* value) {
    return (static_cast<uint32_t>(value[0]) << 24) |
           (static_cast<uint32_t>(value[1]) << 16) |
           (static_cast<uint32_t>(value[2]) << 8) |
           static_cast<uint32_t>(value[3]);
}

uint64_t read_u64be(const uint8_t* value) {
    uint64_t result = 0;
    for (size_t i = 0; i < 8; ++i) result = (result << 8) | value[i];
    return result;
}

#ifdef _WIN32
bool aes_gcm_decrypt(const link_v2_audio_key_material& material,
                     uint64_t sequence,
                     const uint8_t* aad,
                     size_t aad_length,
                     const uint8_t* ciphertext,
                     size_t ciphertext_length,
                     const uint8_t* tag,
                     std::vector<uint8_t>& plaintext) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::array<uint8_t, 12> nonce{};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    ULONG written = 0;
    std::copy(material.nonce_prefix.begin(), material.nonce_prefix.end(), nonce.begin());
    for (size_t i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(sequence >> (56 - i * 8));

    bool success = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) goto cleanup;
    if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) goto cleanup;
    if (BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0,
            const_cast<PUCHAR>(material.audio_key.data()),
            static_cast<ULONG>(material.audio_key.size()), 0) < 0) goto cleanup;

    plaintext.assign(ciphertext_length, 0);
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce.data();
    info.cbNonce = static_cast<ULONG>(nonce.size());
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aad_length);
    info.pbTag = const_cast<PUCHAR>(tag);
    info.cbTag = static_cast<ULONG>(kTagSize);
    if (BCryptDecrypt(key, const_cast<PUCHAR>(ciphertext),
            static_cast<ULONG>(ciphertext_length), &info, nullptr, 0,
            plaintext.data(), static_cast<ULONG>(plaintext.size()), &written, 0) < 0) goto cleanup;
    plaintext.resize(written);
    success = true;

cleanup:
    if (!success) {
        std::fill(plaintext.begin(), plaintext.end(), static_cast<uint8_t>(0));
        plaintext.clear();
    }
    std::fill(nonce.begin(), nonce.end(), static_cast<uint8_t>(0));
    if (key != nullptr) BCryptDestroyKey(key);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}
#endif
}

link_v2_audio_frame_result decode_link_v2_audio_frame(
    const std::vector<uint8_t>& frame,
    link_v2_audio_session_registry& registry,
    std::vector<uint8_t>& plaintext) {
    plaintext.clear();
    if (frame.size() < kHeaderSize + kTagSize ||
        std::memcmp(frame.data(), "ZL2A", 4) != 0 ||
        frame[4] != 2 || frame[5] != 0) return link_v2_audio_frame_result::InvalidFrame;

    std::array<uint8_t, 16> session_id{};
    std::copy(frame.begin() + 6, frame.begin() + 22, session_id.begin());
    const uint64_t sequence = read_u64be(frame.data() + 22);
    const uint32_t length = read_u32be(frame.data() + 30);
    if (sequence == 0 || length > kMaximumPlaintext || (length & 1) != 0 ||
        frame.size() != kHeaderSize + static_cast<size_t>(length) + kTagSize)
        return link_v2_audio_frame_result::InvalidFrame;

    link_v2_audio_key_material material{};
    if (!registry.find_session(session_id, material))
        return link_v2_audio_frame_result::UnknownSession;
    if (sequence <= material.last_sequence) {
        std::fill(material.audio_key.begin(), material.audio_key.end(), static_cast<uint8_t>(0));
        return link_v2_audio_frame_result::Replay;
    }

#ifdef _WIN32
    const bool authenticated = aes_gcm_decrypt(material, sequence,
        frame.data(), kHeaderSize,
        frame.data() + kHeaderSize, length,
        frame.data() + kHeaderSize + length, plaintext);
#else
    const bool authenticated = false;
#endif
    std::fill(material.audio_key.begin(), material.audio_key.end(), static_cast<uint8_t>(0));
    std::fill(material.nonce_prefix.begin(), material.nonce_prefix.end(), static_cast<uint8_t>(0));
    if (!authenticated) return link_v2_audio_frame_result::AuthenticationFailed;
    if (!registry.commit_sequence(session_id, sequence)) {
        std::fill(plaintext.begin(), plaintext.end(), static_cast<uint8_t>(0));
        plaintext.clear();
        return link_v2_audio_frame_result::Replay;
    }
    return link_v2_audio_frame_result::Accepted;
}
