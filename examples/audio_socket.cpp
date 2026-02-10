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
    : m_sample_rate(sample_rate), m_running(false), m_buffer(static_cast<size_t>(sample_rate) * 30) {
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
    std::cout << "[[AUDIO_SOCKET_READY:" << port << "]]" << std::endl;
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

    // 【P3-严重Bug修复】允许端口复用，避免重启后 bind 失败
    int optval = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));

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

        // 【P3-严重Bug修复】设置 recv 超时 5 秒
        // WiFi 断开时，recv() 不会永远阻塞，最多 5 秒后返回错误
        // 然后 handle_client 退出，server_thread 可以回到 accept() 接受新连接
        DWORD timeout = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

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

    std::vector<float> samples(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        samples[i] = static_cast<float>(data[i]) / 32768.0f;
    }

    m_buffer.push(samples);
}

void audio_socket::get(int ms, std::vector<float>& audio) {
    size_t samples_needed = (static_cast<size_t>(m_sample_rate) * ms) / 1000;
    m_buffer.read(samples_needed, audio, 20);
}

void audio_socket::clear() {
    m_buffer.clear();
}

void audio_socket::set_idle(bool idle) {
    m_buffer.set_overwrite_oldest(idle);
}
