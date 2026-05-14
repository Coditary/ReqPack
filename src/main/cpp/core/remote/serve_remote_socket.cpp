#include "serve_remote_internal.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstring>

namespace {

volatile std::sig_atomic_t g_remote_signal_shutdown_requested = 0;
volatile std::sig_atomic_t g_remote_signal_server_fd = -1;

void handle_remote_serve_signal(int) {
    g_remote_signal_shutdown_requested = 1;
    const int serverFd = static_cast<int>(g_remote_signal_server_fd);
    if (serverFd != -1) {
        ::shutdown(serverFd, SHUT_RDWR);
        ::close(serverFd);
        g_remote_signal_server_fd = -1;
    }
}

}  // namespace

ScopedRemoteSignalHandlers::ScopedRemoteSignalHandlers(int serverFd) {
    g_remote_signal_shutdown_requested = 0;
    g_remote_signal_server_fd = serverFd;

    struct sigaction action {};
    action.sa_handler = handle_remote_serve_signal;
    sigemptyset(&action.sa_mask);

    if (::sigaction(SIGTERM, &action, &oldTerm_) != 0) {
        return;
    }
    if (::sigaction(SIGINT, &action, &oldInt_) != 0) {
        ::sigaction(SIGTERM, &oldTerm_, nullptr);
        return;
    }
    installed_ = true;
}

ScopedRemoteSignalHandlers::~ScopedRemoteSignalHandlers() {
    g_remote_signal_server_fd = -1;
    g_remote_signal_shutdown_requested = 0;
    if (installed_) {
        ::sigaction(SIGTERM, &oldTerm_, nullptr);
        ::sigaction(SIGINT, &oldInt_, nullptr);
    }
}

bool ScopedRemoteSignalHandlers::shutdownRequested() const {
    return g_remote_signal_shutdown_requested != 0;
}

bool send_all(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::send(fd, data.data() + offset, data.size() - offset, 0);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool read_exact_bytes(int fd, char* buffer, std::size_t count) {
    std::size_t offset = 0;
    while (offset < count) {
        const ssize_t received = ::recv(fd, buffer + offset, count - offset, 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

bool discard_bytes(int fd, std::uintmax_t count) {
    char buffer[8192];
    std::uintmax_t remaining = count;
    while (remaining > 0) {
        const std::size_t chunkSize = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, sizeof(buffer)));
        if (!read_exact_bytes(fd, buffer, chunkSize)) {
            return false;
        }
        remaining -= chunkSize;
    }
    return true;
}

std::optional<std::string> read_line_from_socket(int fd) {
    std::string line;
    char c = '\0';
    for (;;) {
        const ssize_t received = ::recv(fd, &c, 1, 0);
        if (received == 0) {
            if (line.empty()) {
                return std::nullopt;
            }
            break;
        }
        if (received < 0) {
            return std::nullopt;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
    return line;
}

std::optional<ConnectionProtocol> detect_connection_protocol(
    const ServeRuntimeOptions& options,
    const std::string& firstLine
) {
    if (options.remoteProtocol == ServeRemoteProtocol::JSON) {
        return ConnectionProtocol::JSON;
    }
    if (options.remoteProtocol == ServeRemoteProtocol::TEXT) {
        const std::string trimmed = trim_copy(firstLine);
        if (!trimmed.empty() && trimmed.front() == '{' && trimmed.back() == '}') {
            return ConnectionProtocol::JSON;
        }
        return ConnectionProtocol::TEXT;
    }
    return std::nullopt;
}

int create_server_socket(const ServeRuntimeOptions& options, Logger& logger) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* addresses = nullptr;
    const std::string portString = std::to_string(options.port);
    if (::getaddrinfo(options.bind.c_str(), portString.c_str(), &hints, &addresses) != 0) {
        logger.err("failed to resolve bind address '" + options.bind + "'");
        return -1;
    }

    int serverFd = -1;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        serverFd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (serverFd == -1) {
            continue;
        }

        int reuse = 1;
        (void)::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (::bind(serverFd, address->ai_addr, address->ai_addrlen) == 0 && ::listen(serverFd, SOMAXCONN) == 0) {
            break;
        }

        ::close(serverFd);
        serverFd = -1;
    }

    ::freeaddrinfo(addresses);

    if (serverFd == -1) {
        logger.err("failed to bind remote server on " + options.bind + ":" + portString);
    }
    return serverFd;
}
