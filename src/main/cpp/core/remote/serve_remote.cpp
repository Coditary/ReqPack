#include "core/remote/serve_remote.h"

#include "serve_remote_internal.h"

#include "core/remote/remote_profiles.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

bool remote_protocol_requires_explicit_mode(ServeRemoteProtocol protocol) {
    return protocol == ServeRemoteProtocol::HTTP || protocol == ServeRemoteProtocol::HTTPS;
}

int run_remote_serve(
    Cli& cli,
    const ReqPackConfig& config,
    const std::filesystem::path& configPath,
    const ReqPackConfigOverrides& configOverrides,
    Logger& logger,
    IDisplay* display,
    const ServeRuntimeOptions& options
) {
    if (options.remoteProtocol == ServeRemoteProtocol::HTTP) {
        logger.err("serve --remote --http is not implemented yet");
        logger.flushSync();
        return 1;
    }
    if (options.remoteProtocol == ServeRemoteProtocol::HTTPS) {
        logger.err("serve --remote --https is not implemented yet");
        logger.flushSync();
        return 1;
    }

    const int serverFd = create_server_socket(options, logger);
    if (serverFd == -1) {
        logger.flushSync();
        return 1;
    }

    ScopedRemoteSignalHandlers signalHandlers(serverFd);

    render_command_output(CommandOutput{
        .mode = DisplayMode::SERVE,
        .sessionItems = {"remote-server"},
        .blocks = {make_command_field_value_block({
            {.key = "Bind", .value = options.bind},
            {.key = "Port", .value = std::to_string(options.port)},
            {.key = "Protocol", .value = options.remoteProtocol == ServeRemoteProtocol::JSON ? "json" : "text"},
            {.key = "Readonly", .value = options.readonly ? "true" : "false"},
            {.key = "Max Connections", .value = std::to_string(options.maxConnections)},
        })},
        .success = true,
        .succeeded = 1,
    });
    logger.flushSync();

    RemoteServerState state{
        .config = config,
        .options = options,
        .users = load_remote_users(default_remote_profiles_path()),
        .configPath = configPath,
        .configOverrides = configOverrides,
        .remoteUsersPath = default_remote_profiles_path(),
        .serverFd = serverFd,
    };
    std::mutex commandMutex;

    while (!state.shutdownRequested.load() && !signalHandlers.shutdownRequested()) {
        sockaddr_storage clientAddress{};
        socklen_t clientLength = sizeof(clientAddress);
        const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        if (clientFd == -1) {
            if (state.shutdownRequested.load() || signalHandlers.shutdownRequested()) {
                break;
            }
            continue;
        }

        const RemoteStateSnapshot snapshot = snapshot_remote_state(state);
        if (active_connection_count(state) >= snapshot.options.maxConnections) {
            const CommandOutput output = command_output_message(DisplayMode::SERVE, "max connections reached", false);
            const std::string response = snapshot.options.remoteProtocol == ServeRemoteProtocol::JSON
                ? json_response(false, output)
                : text_response(false, output);
            (void)send_all(clientFd, response);
            ::close(clientFd);
            continue;
        }

        char hostBuffer[NI_MAXHOST];
        char serviceBuffer[NI_MAXSERV];
        std::string remoteAddress = "unknown";
        if (::getnameinfo(
                reinterpret_cast<sockaddr*>(&clientAddress),
                clientLength,
                hostBuffer,
                sizeof(hostBuffer),
                serviceBuffer,
                sizeof(serviceBuffer),
                NI_NUMERICHOST | NI_NUMERICSERV
            ) == 0) {
            remoteAddress = std::string(hostBuffer) + ":" + serviceBuffer;
        }

        int sessionId = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            sessionId = state.nextSessionId++;
            state.sessions.emplace(sessionId, RemoteSessionInfo{
                .id = sessionId,
                .remoteAddress = remoteAddress,
                .connectedAt = std::chrono::system_clock::now(),
            });
        }

        std::thread([&, clientFd, sessionId]() {
            const std::optional<std::string> firstLine = read_line_from_socket(clientFd);
            if (firstLine.has_value()) {
                const RemoteStateSnapshot workerSnapshot = snapshot_remote_state(state);
                const std::optional<ConnectionProtocol> protocol = detect_connection_protocol(workerSnapshot.options, firstLine.value());
                if (protocol.has_value() && protocol.value() == ConnectionProtocol::JSON) {
                    set_session_protocol(state, sessionId, ConnectionProtocol::JSON);
                    handle_json_client(clientFd, cli, state, logger, display, sessionId, commandMutex, firstLine);
                } else if (protocol.has_value() && protocol.value() == ConnectionProtocol::TEXT) {
                    set_session_protocol(state, sessionId, ConnectionProtocol::TEXT);
                    handle_text_client(clientFd, cli, state, logger, display, sessionId, commandMutex, firstLine);
                } else {
                    (void)send_all(clientFd, text_response(false, command_output_message(DisplayMode::SERVE, "unsupported negotiated protocol", false)));
                }
            }
            ::close(clientFd);
            std::lock_guard<std::mutex> lock(state.mutex);
            state.sessions.erase(sessionId);
        }).detach();
    }

    if (signalHandlers.shutdownRequested()) {
        state.shutdownRequested.store(true);
        std::lock_guard<std::mutex> lock(state.mutex);
        state.serverFd = -1;
    }

    logger.flushSync();
    return 0;
}
