#pragma once

#include "core/remote/serve_remote.h"
#include "core/remote/remote_profiles.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <signal.h>
#include <string>
#include <vector>

inline constexpr const char* REMOTE_UPLOAD_INSTALL_COMMAND = "__reqpack_upload_install__";
inline constexpr const char* REMOTE_UPLOAD_PATH_PLACEHOLDER = "__REQPACK_REMOTE_UPLOAD_PATH__";

struct RemoteResponse {
    bool ok{false};
    CommandOutput output{};
    bool closeConnection{false};
};

struct UploadInstallEnvelope {
    std::uintmax_t size{0};
    std::string filename;
    std::string commandTemplate;
};

struct JsonCommand {
    std::string command;
    std::optional<std::string> token;
    std::optional<std::string> username;
    std::optional<std::string> password;
};

enum class ConnectionProtocol {
    TEXT,
    JSON
};

struct SessionIdentity {
    bool authenticated{false};
    std::string userId;
    bool isAdmin{false};
    std::string authType{"none"};
};

struct RemoteSessionInfo {
    int id{0};
    std::string remoteAddress;
    ConnectionProtocol protocol{ConnectionProtocol::TEXT};
    std::string userId;
    bool isAdmin{false};
    std::string authType{"none"};
    std::chrono::system_clock::time_point connectedAt{std::chrono::system_clock::now()};
};

struct RemoteStateSnapshot {
    ReqPackConfig config;
    ServeRuntimeOptions options;
    std::vector<RemoteUser> users;
};

struct RemoteServerState {
    std::mutex mutex;
    ReqPackConfig config;
    ServeRuntimeOptions options;
    std::vector<RemoteUser> users;
    std::map<int, RemoteSessionInfo> sessions;
    std::filesystem::path configPath;
    ReqPackConfigOverrides configOverrides;
    std::filesystem::path remoteUsersPath;
    int serverFd{-1};
    int nextSessionId{1};
    std::atomic<bool> shutdownRequested{false};
};

inline std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::vector<std::string> tokenize_command_line(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuotes = false;
    bool inDoubleQuotes = false;
    bool escaping = false;

    for (char c : command) {
        if (escaping) {
            current.push_back(c);
            escaping = false;
            continue;
        }

        if (c == '\\' && !inSingleQuotes) {
            escaping = true;
            continue;
        }

        if (c == '\'' && !inDoubleQuotes) {
            inSingleQuotes = !inSingleQuotes;
            continue;
        }

        if (c == '"' && !inSingleQuotes) {
            inDoubleQuotes = !inDoubleQuotes;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c)) && !inSingleQuotes && !inDoubleQuotes) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(c);
    }

    if (escaping || inSingleQuotes || inDoubleQuotes) {
        return {};
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}

inline std::vector<std::string> merged_command_arguments(
    const std::vector<std::string>& commandTokens,
    const std::vector<std::string>& inheritedArguments
) {
    std::vector<std::string> merged;
    merged.reserve(commandTokens.size() + inheritedArguments.size());
    merged.insert(merged.end(), commandTokens.begin(), commandTokens.end());
    merged.insert(merged.end(), inheritedArguments.begin(), inheritedArguments.end());
    return merged;
}

class ScopedRemoteSignalHandlers {
public:
    explicit ScopedRemoteSignalHandlers(int serverFd);
    ~ScopedRemoteSignalHandlers();

    bool shutdownRequested() const;

private:
    struct sigaction oldTerm_ {};
    struct sigaction oldInt_ {};
    bool installed_{false};
};

bool send_all(int fd, const std::string& data);
bool read_exact_bytes(int fd, char* buffer, std::size_t count);
bool discard_bytes(int fd, std::uintmax_t count);
std::optional<std::string> read_line_from_socket(int fd);
std::optional<ConnectionProtocol> detect_connection_protocol(
    const ServeRuntimeOptions& options,
    const std::string& firstLine
);
int create_server_socket(const ServeRuntimeOptions& options, Logger& logger);

RemoteStateSnapshot snapshot_remote_state(RemoteServerState& state);
std::string connection_protocol_name(ConnectionProtocol protocol);
std::string format_timestamp(const std::chrono::system_clock::time_point& timePoint);
CommandOutput command_output_message(DisplayMode mode, const std::string& message, bool success = true);
CommandOutput active_connection_count_output(RemoteServerState& state);
CommandOutput active_connection_list_output(RemoteServerState& state);
int active_connection_count(RemoteServerState& state);
void request_server_shutdown(RemoteServerState& state);
void update_session_identity(RemoteServerState& state, int sessionId, const SessionIdentity& identity);
void set_session_protocol(RemoteServerState& state, int sessionId, ConnectionProtocol protocol);

bool has_user_registry(const RemoteStateSnapshot& snapshot);
bool auth_required(const RemoteStateSnapshot& snapshot);
bool authenticate_text_command(
    RemoteServerState& state,
    int sessionId,
    const std::string& line,
    SessionIdentity& identity,
    RemoteResponse& response
);
bool authenticate_json_command(
    RemoteServerState& state,
    int sessionId,
    const JsonCommand& command,
    SessionIdentity& identity,
    RemoteResponse& response
);

std::optional<UploadInstallEnvelope> parse_upload_install_envelope(const std::vector<std::string>& commandTokens);
std::string sanitize_upload_filename(std::string filename);

class ScopedPathCleanup {
public:
    ScopedPathCleanup() = default;
    explicit ScopedPathCleanup(std::filesystem::path path);
    ~ScopedPathCleanup();

    ScopedPathCleanup(const ScopedPathCleanup&) = delete;
    ScopedPathCleanup& operator=(const ScopedPathCleanup&) = delete;
    ScopedPathCleanup(ScopedPathCleanup&& other) noexcept;
    ScopedPathCleanup& operator=(ScopedPathCleanup&& other) noexcept;

    const std::filesystem::path& path() const;

private:
    void reset();

    std::filesystem::path path_;
};

ScopedPathCleanup write_uploaded_file_to_temp(int clientFd, const UploadInstallEnvelope& envelope);
std::string substitute_upload_path(const std::string& commandTemplate, const std::filesystem::path& path);

bool reload_remote_state(RemoteServerState& state, Logger& logger, std::string& error);
bool requests_allowed_in_readonly_mode(const std::vector<Request>& requests);
bool command_requires_close(const std::string& command);
RemoteResponse execute_command(
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    const SessionIdentity& identity,
    const std::string& commandLine,
    std::mutex& commandMutex
);
RemoteResponse execute_upload_install_command(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    const SessionIdentity& identity,
    const std::vector<std::string>& commandTokens,
    std::mutex& commandMutex
);

std::optional<JsonCommand> parse_json_command(const std::string& line);
std::string json_response(bool ok, const CommandOutput& output);
std::string text_response(bool ok, const CommandOutput& output);
void handle_text_client(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    int sessionId,
    std::mutex& commandMutex,
    std::optional<std::string> pendingLine = std::nullopt
);
void handle_json_client(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    int sessionId,
    std::mutex& commandMutex,
    std::optional<std::string> pendingLine = std::nullopt
);
