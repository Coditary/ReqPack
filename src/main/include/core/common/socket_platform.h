#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

using ReqpackSocket = SOCKET;
inline constexpr ReqpackSocket REQPACK_INVALID_SOCKET = INVALID_SOCKET;

inline void reqpack_ensure_socket_runtime() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsa_data{};
        (void)::WSAStartup(MAKEWORD(2, 2), &wsa_data);
        initialized = true;
    }
}

inline void reqpack_close_socket(ReqpackSocket socket) {
    if (socket != INVALID_SOCKET) {
        ::closesocket(socket);
    }
}

inline int reqpack_shutdown_socket(ReqpackSocket socket) {
    return ::shutdown(socket, SD_BOTH);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

using ReqpackSocket = int;
inline constexpr ReqpackSocket REQPACK_INVALID_SOCKET = -1;

inline void reqpack_ensure_socket_runtime() {}

inline void reqpack_close_socket(ReqpackSocket socket) {
    if (socket >= 0) {
        ::close(socket);
    }
}

inline int reqpack_shutdown_socket(ReqpackSocket socket) {
    return ::shutdown(socket, SHUT_RDWR);
}
#endif
