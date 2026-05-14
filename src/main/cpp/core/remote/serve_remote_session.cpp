#include "serve_remote_internal.h"

#include <sys/socket.h>
#include <unistd.h>

#include <ctime>
#include <iomanip>
#include <sstream>

std::string connection_protocol_name(ConnectionProtocol protocol) {
    return protocol == ConnectionProtocol::JSON ? "json" : "text";
}

std::string format_timestamp(const std::chrono::system_clock::time_point& timePoint) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

RemoteStateSnapshot snapshot_remote_state(RemoteServerState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return RemoteStateSnapshot{.config = state.config, .options = state.options, .users = state.users};
}

CommandOutput command_output_message(DisplayMode mode, const std::string& message, bool success) {
    CommandOutput output;
    output.mode = mode;
    output.sessionItems = {"remote"};
    if (!message.empty()) {
        output.blocks.push_back(make_command_message_block(message));
    }
    output.success = success;
    output.succeeded = success ? 1 : 0;
    output.failed = success ? 0 : 1;
    return output;
}

CommandOutput active_connection_count_output(RemoteServerState& state) {
    CommandOutput output;
    output.mode = DisplayMode::SERVE;
    output.sessionItems = {"connections"};
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        count = static_cast<int>(state.sessions.size());
    }
    output.blocks.push_back(make_command_field_value_block(std::vector<CommandOutputField>{
        CommandOutputField{.key = "Active Connections", .value = std::to_string(count)}
    }));
    output.success = true;
    output.succeeded = 1;
    return output;
}

CommandOutput active_connection_list_output(RemoteServerState& state) {
    CommandOutput output;
    output.mode = DisplayMode::SERVE;
    output.sessionItems = {"connections"};
    std::vector<std::vector<std::string>> rows;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (const auto& [_, session] : state.sessions) {
            rows.push_back({
                std::to_string(session.id),
                session.userId.empty() ? "-" : session.userId,
                session.isAdmin ? "true" : "false",
                session.authType,
                connection_protocol_name(session.protocol),
                session.remoteAddress,
                format_timestamp(session.connectedAt),
            });
        }
    }
    output.blocks.push_back(make_command_table_block(
        {"Id", "User", "Admin", "Auth", "Protocol", "Address", "Connected"},
        rows));
    output.success = true;
    output.succeeded = static_cast<int>(rows.size());
    return output;
}

int active_connection_count(RemoteServerState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return static_cast<int>(state.sessions.size());
}

void request_server_shutdown(RemoteServerState& state) {
    int serverFd = -1;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        serverFd = state.serverFd;
        state.serverFd = -1;
    }
    if (serverFd != -1) {
        ::shutdown(serverFd, SHUT_RDWR);
        ::close(serverFd);
    }
}

void update_session_identity(RemoteServerState& state, int sessionId, const SessionIdentity& identity) {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.sessions.find(sessionId);
    if (it == state.sessions.end()) {
        return;
    }
    it->second.userId = identity.userId;
    it->second.isAdmin = identity.isAdmin;
    it->second.authType = identity.authType;
}

void set_session_protocol(RemoteServerState& state, int sessionId, ConnectionProtocol protocol) {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.sessions.find(sessionId);
    if (it != state.sessions.end()) {
        it->second.protocol = protocol;
    }
}
