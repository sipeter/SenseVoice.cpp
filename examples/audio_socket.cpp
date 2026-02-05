#include "audio_socket.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

extern std::atomic<bool> g_should_exit;

audio_socket::audio_socket(int sample_rate)
    : m_sample_rate(sample_rate), m_running(false) {
}

audio_socket::~audio_socket() {
    stop();
}

bool audio_socket::init(int port) {
    if (m_running) return true;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[audio_socket] WSAStartup 失败" << std::endl;
        return false;
    }
#endif

    m_running = true;
    m_thread = std::thread(&audio_socket::server_thread, this, port);
    std::cerr << "[audio_socket] 监听端口: " << port << std::endl;
    return true;
}

void audio_socket::stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
#ifdef _WIN32
    WSACleanup();
#endif
}

void audio_socket::server_thread(int port) {
#ifdef _WIN32
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        std::cerr << "[audio_socket] 创建 socket 失败" << std::endl;
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(port));

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[audio_socket] 绑定端口失败" << std::endl;
        closesocket(listenSock);
        return;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR) {
        std::cerr << "[audio_socket] 监听失败" << std::endl;
        closesocket(listenSock);
        return;
    }

    while (m_running && !g_should_exit) {
        sockaddr_in clientAddr{};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            std::cerr << "[audio_socket] accept 失败" << std::endl;
            continue;
        }

        std::cerr << "[audio_socket] 客户端已连接" << std::endl;
        handle_client(static_cast<int>(clientSock));
        closesocket(clientSock);
        std::cerr << "[audio_socket] 客户端已断开" << std::endl;
    }

    closesocket(listenSock);
#else
    (void)port;
    std::cerr << "[audio_socket] 非 Windows 平台未实现" << std::endl;
#endif
}

void audio_socket::handle_client(int client_socket) {
#ifdef _WIN32
    const int bufferSize = 4096;
    std::vector<char> buffer(bufferSize);

    while (m_running && !g_should_exit) {
        int received = recv(client_socket, buffer.data(), bufferSize, 0);
        if (received <= 0) {
            break;
        }

        size_t sample_count = static_cast<size_t>(received / 2);
        if (sample_count > 0) {
            push_pcm16(reinterpret_cast<const int16_t*>(buffer.data()), sample_count);
        }
    }
#else
    (void)client_socket;
#endif
}

void audio_socket::push_pcm16(const int16_t* data, size_t sample_count) {
    if (sample_count == 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (size_t i = 0; i < sample_count; ++i) {
        float sample = static_cast<float>(data[i]) / 32768.0f;
        m_buffer.push_back(sample);
    }

    const size_t max_samples = static_cast<size_t>(m_sample_rate) * 30;
    if (m_buffer.size() > max_samples) {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + (m_buffer.size() - max_samples));
    }
}

void audio_socket::get(int ms, std::vector<float>& audio) {
    std::lock_guard<std::mutex> lock(m_mutex);

    size_t samples_needed = (static_cast<size_t>(m_sample_rate) * ms) / 1000;
    if (m_buffer.size() >= samples_needed) {
        audio.assign(m_buffer.begin(), m_buffer.begin() + samples_needed);
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + samples_needed);
    } else if (!m_buffer.empty()) {
        audio.assign(m_buffer.begin(), m_buffer.end());
        m_buffer.clear();
    } else {
        audio.clear();
    }
}

void audio_socket::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffer.clear();
}
