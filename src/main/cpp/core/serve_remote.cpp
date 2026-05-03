#include "core/serve_remote.h"

#include "core/orchestrator.h"
#include "core/remote_profiles.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

namespace {

constexpr const char* REMOTE_UPLOAD_INSTALL_COMMAND = "__reqpack_upload_install__";
constexpr const char* REMOTE_UPLOAD_PATH_PLACEHOLDER = "__REQPACK_REMOTE_UPLOAD_PATH__";

struct RemoteResponse {
    bool ok{false};
    std::string body;
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

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> tokenize_command_line(const std::string& command) {
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

std::vector<std::string> merged_command_arguments(
    const std::vector<std::string>& commandTokens,
    const std::vector<std::string>& inheritedArguments
) {
    std::vector<std::string> merged;
    merged.reserve(commandTokens.size() + inheritedArguments.size());
    merged.insert(merged.end(), commandTokens.begin(), commandTokens.end());
    merged.insert(merged.end(), inheritedArguments.begin(), inheritedArguments.end());
    return merged;
}

std::optional<UploadInstallEnvelope> parse_upload_install_envelope(const std::vector<std::string>& commandTokens) {
    if (commandTokens.size() < 4 || commandTokens[0] != REMOTE_UPLOAD_INSTALL_COMMAND) {
        return std::nullopt;
    }

    try {
        const unsigned long long parsedSize = std::stoull(commandTokens[1]);
        UploadInstallEnvelope envelope;
        envelope.size = static_cast<std::uintmax_t>(parsedSize);
        envelope.filename = commandTokens[2];
        envelope.commandTemplate = commandTokens[3];
        if (envelope.filename.empty() || envelope.commandTemplate.empty()) {
            return std::nullopt;
        }
        return envelope;
    } catch (...) {
        return std::nullopt;
    }
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

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string json_string_field(const std::string& key, const std::string& value) {
    return "\"" + escape_json(key) + "\":\"" + escape_json(value) + "\"";
}

std::optional<std::string> extract_json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t start = pos + needle.size();
    std::string value;
    bool escaped = false;
    for (std::size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            return value;
        }
        value += c;
    }
    return std::nullopt;
}

std::optional<JsonCommand> parse_json_command(const std::string& line) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed.front() != '{' || trimmed.back() != '}') {
        return std::nullopt;
    }

    JsonCommand command;
    command.command = extract_json_string_field(trimmed, "command").value_or(std::string{});
    command.token = extract_json_string_field(trimmed, "token");
    command.username = extract_json_string_field(trimmed, "username");
    command.password = extract_json_string_field(trimmed, "password");
    return command;
}

std::string json_response(bool ok, const std::string& body) {
    if (ok) {
        return std::string{"{"} + "\"ok\":true," + json_string_field("output", body) + "}\n";
    }
    return std::string{"{"} + "\"ok\":false," + json_string_field("error", body) + "}\n";
}

std::string text_response(bool ok, const std::string& body) {
    return std::string(ok ? "OK " : "ERR ") + std::to_string(body.size()) + "\n" + body;
}

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

std::string list_active_connections(RemoteServerState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    std::ostringstream output;
    for (const auto& [_, session] : state.sessions) {
        output << "id=" << session.id
               << " user=" << (session.userId.empty() ? "-" : session.userId)
               << " admin=" << (session.isAdmin ? "true" : "false")
               << " auth=" << session.authType
               << " protocol=" << connection_protocol_name(session.protocol)
               << " addr=" << session.remoteAddress
               << " connected=" << format_timestamp(session.connectedAt)
               << '\n';
    }
    return output.str();
}

int active_connection_count(RemoteServerState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return static_cast<int>(state.sessions.size());
}

bool has_user_registry(const RemoteStateSnapshot& snapshot) {
    return !snapshot.users.empty();
}

std::optional<SessionIdentity> resolve_remote_user_by_token(
    const RemoteStateSnapshot& snapshot,
    const std::string& token
) {
    for (const RemoteUser& user : snapshot.users) {
        if (user.token.has_value() && user.token.value() == token) {
            return SessionIdentity{.authenticated = true, .userId = user.id, .isAdmin = user.isAdmin, .authType = "token"};
        }
    }
    return std::nullopt;
}

std::optional<SessionIdentity> resolve_remote_user_by_basic(
    const RemoteStateSnapshot& snapshot,
    const std::string& username,
    const std::string& password
) {
    for (const RemoteUser& user : snapshot.users) {
        const std::string loginName = user.username.value_or(user.id);
        if (user.password.has_value() && loginName == username && user.password.value() == password) {
            return SessionIdentity{.authenticated = true, .userId = user.id, .isAdmin = user.isAdmin, .authType = "basic"};
        }
    }
    return std::nullopt;
}

std::optional<SessionIdentity> resolve_fallback_identity_by_token(
    const RemoteStateSnapshot& snapshot,
    const std::string& token
) {
    if (snapshot.options.token.has_value() && snapshot.options.token.value() == token) {
        return SessionIdentity{.authenticated = true, .userId = "remote", .isAdmin = false, .authType = "fallback-token"};
    }
    return std::nullopt;
}

std::optional<SessionIdentity> resolve_fallback_identity_by_basic(
    const RemoteStateSnapshot& snapshot,
    const std::string& username,
    const std::string& password
) {
    if (snapshot.options.username.has_value() && snapshot.options.password.has_value() &&
        snapshot.options.username.value() == username && snapshot.options.password.value() == password) {
        return SessionIdentity{.authenticated = true, .userId = username, .isAdmin = false, .authType = "fallback-basic"};
    }
    return std::nullopt;
}

bool auth_required(const RemoteStateSnapshot& snapshot) {
    return has_user_registry(snapshot) || snapshot.options.token.has_value() ||
        (snapshot.options.username.has_value() && snapshot.options.password.has_value());
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

bool reload_remote_state(RemoteServerState& state, Logger& logger, std::string& error) {
    try {
        ReqPackConfig config = load_config_from_lua(state.configPath, DEFAULT_REQPACK_CONFIG);
        config = apply_config_overrides(config, state.configOverrides);
        const std::filesystem::path workspacePluginDirectory = std::filesystem::current_path() / "plugins";
        if (!state.configOverrides.pluginDirectory.has_value() && std::filesystem::exists(workspacePluginDirectory)) {
            config.registry.pluginDirectory = workspacePluginDirectory.string();
        }

        ServeRuntimeOptions options;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            options = state.options;
        }
        if (!options.readonlyExplicit) {
            options.readonly = config.remote.readonly;
        }
        if (!options.maxConnectionsExplicit) {
            options.maxConnections = config.remote.maxConnections;
        }

        const std::vector<RemoteUser> users = load_remote_users(state.remoteUsersPath);

        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.config = config;
            state.options = options;
            state.users = users;
        }

        logger.setLevel(to_string(config.logging.level));
        logger.setPattern(config.logging.pattern);
        logger.setBacktrace(config.logging.enableBacktrace, config.logging.backtraceSize);
        if (config.logging.fileOutput) {
            logger.setFileSink(config.logging.filePath);
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
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

bool is_readonly_safe_action(ActionType action) {
    return action == ActionType::LIST || action == ActionType::SEARCH || action == ActionType::INFO ||
        action == ActionType::OUTDATED || action == ActionType::SBOM;
}

bool requests_allowed_in_readonly_mode(const std::vector<Request>& requests) {
    if (requests.empty()) {
        return false;
    }
    for (const Request& request : requests) {
        if (!is_readonly_safe_action(request.action)) {
            return false;
        }
        if (!request.outputPath.empty()) {
            return false;
        }
    }
    return true;
}

bool command_requires_close(const std::string& command) {
    const std::string trimmed = trim_copy(command);
    return trimmed == "quit" || trimmed == "exit";
}

std::string sanitize_upload_filename(std::string filename) {
    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty()) {
        return "upload.bin";
    }
    for (char& c : filename) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (!std::isalnum(byte) && c != '.' && c != '_' && c != '-') {
            c = '_';
        }
    }
    return filename;
}

class ScopedPathCleanup {
public:
    ScopedPathCleanup() = default;

    explicit ScopedPathCleanup(std::filesystem::path path)
        : path_(std::move(path)) {
    }

    ~ScopedPathCleanup() {
        reset();
    }

    ScopedPathCleanup(const ScopedPathCleanup&) = delete;
    ScopedPathCleanup& operator=(const ScopedPathCleanup&) = delete;

    ScopedPathCleanup(ScopedPathCleanup&& other) noexcept
        : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    ScopedPathCleanup& operator=(ScopedPathCleanup&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        path_ = std::move(other.path_);
        other.path_.clear();
        return *this;
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    void reset() {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove(path_, error);
        path_.clear();
    }

    std::filesystem::path path_;
};

ScopedPathCleanup write_uploaded_file_to_temp(int clientFd, const UploadInstallEnvelope& envelope) {
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "reqpack" / "remote-upload";
    std::error_code dirError;
    std::filesystem::create_directories(tempRoot, dirError);

    const std::string filename = sanitize_upload_filename(envelope.filename);
    const std::string uniquePrefix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path tempPath = tempRoot / (uniquePrefix + "-" + filename);

    ScopedPathCleanup cleanup(tempPath);
    std::optional<std::string> streamError;
    std::ofstream output;
    if (dirError) {
        streamError = "failed to create temp upload directory";
    } else {
        output.open(tempPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            streamError = "failed to open temp upload file";
        }
    }

    std::uintmax_t remaining = envelope.size;
    char buffer[8192];
    while (remaining > 0) {
        const std::size_t chunkSize = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, sizeof(buffer)));
        if (!read_exact_bytes(clientFd, buffer, chunkSize)) {
            throw std::runtime_error("failed to read upload payload");
        }
        if (!streamError.has_value()) {
            output.write(buffer, static_cast<std::streamsize>(chunkSize));
            if (!output.good()) {
                streamError = "failed to write temp upload file";
                output.close();
            }
        }
        remaining -= chunkSize;
    }

    if (output.is_open()) {
        output.close();
        if (!streamError.has_value() && !output) {
            streamError = "failed to finalize temp upload file";
        }
    }

    if (streamError.has_value()) {
        throw std::runtime_error(streamError.value());
    }

    return cleanup;
}

std::string substitute_upload_path(const std::string& commandTemplate, const std::filesystem::path& path) {
    const std::string placeholder = REMOTE_UPLOAD_PATH_PLACEHOLDER;
    const std::size_t pos = commandTemplate.find(placeholder);
    if (pos == std::string::npos) {
        throw std::runtime_error("invalid upload command template");
    }

    std::string command = commandTemplate;
    command.replace(pos, placeholder.size(), path.string());
    return command;
}

class StdIoCapture {
public:
    StdIoCapture() {
        std::fflush(stdout);
        std::fflush(stderr);
        oldStdout_ = ::dup(STDOUT_FILENO);
        oldStderr_ = ::dup(STDERR_FILENO);
        file_ = std::tmpfile();
        if (oldStdout_ == -1 || oldStderr_ == -1 || file_ == nullptr) {
            restore();
            throw std::runtime_error("failed to start stdio capture");
        }
        const int captureFd = ::fileno(file_);
        if (::dup2(captureFd, STDOUT_FILENO) == -1 || ::dup2(captureFd, STDERR_FILENO) == -1) {
            restore();
            throw std::runtime_error("failed to redirect stdio");
        }
    }

    ~StdIoCapture() {
        restore();
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    std::string finish() {
        std::fflush(stdout);
        std::fflush(stderr);
        if (file_ == nullptr) {
            return {};
        }
        std::rewind(file_);
        std::ostringstream buffer;
        char chunk[4096];
        while (std::fgets(chunk, static_cast<int>(sizeof(chunk)), file_) != nullptr) {
            buffer << chunk;
        }
        restore();
        return buffer.str();
    }

private:
    void restore() {
        if (oldStdout_ != -1) {
            (void)::dup2(oldStdout_, STDOUT_FILENO);
            ::close(oldStdout_);
            oldStdout_ = -1;
        }
        if (oldStderr_ != -1) {
            (void)::dup2(oldStderr_, STDERR_FILENO);
            ::close(oldStderr_);
            oldStderr_ = -1;
        }
    }

    FILE* file_{nullptr};
    int oldStdout_{-1};
    int oldStderr_{-1};
};

class DisplayGuard {
public:
    DisplayGuard(Logger& logger, IDisplay* restoreDisplay)
        : logger_(logger), restoreDisplay_(restoreDisplay) {
        logger_.setDisplay(nullptr);
    }

    ~DisplayGuard() {
        logger_.setDisplay(restoreDisplay_);
    }

private:
    Logger& logger_;
    IDisplay* restoreDisplay_;
};

RemoteResponse execute_command(
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    const SessionIdentity& identity,
    const std::string& commandLine,
    std::mutex& commandMutex
) {
    const std::string trimmed = trim_copy(commandLine);
    if (trimmed.empty()) {
        return RemoteResponse{.ok = true, .body = {}};
    }
    if (command_requires_close(trimmed)) {
        return RemoteResponse{.ok = true, .body = {}, .closeConnection = true};
    }

    const std::vector<std::string> commandTokens = tokenize_command_line(trimmed);
    if (commandTokens.empty()) {
        return RemoteResponse{.ok = false, .body = "invalid command syntax"};
    }

    if (commandTokens[0] == "shutdown") {
        if (!identity.isAdmin) {
            return RemoteResponse{.ok = false, .body = "admin privileges required"};
        }
        state.shutdownRequested.store(true);
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
        return RemoteResponse{.ok = true, .body = "server shutting down", .closeConnection = true};
    }
    if (commandTokens.size() == 2 && commandTokens[0] == "connections" && commandTokens[1] == "count") {
        if (!identity.isAdmin) {
            return RemoteResponse{.ok = false, .body = "admin privileges required"};
        }
        return RemoteResponse{.ok = true, .body = std::to_string(active_connection_count(state))};
    }
    if (commandTokens.size() == 2 && commandTokens[0] == "connections" && commandTokens[1] == "list") {
        if (!identity.isAdmin) {
            return RemoteResponse{.ok = false, .body = "admin privileges required"};
        }
        return RemoteResponse{.ok = true, .body = list_active_connections(state)};
    }
    if (commandTokens[0] == "reload-config") {
        if (!identity.isAdmin) {
            return RemoteResponse{.ok = false, .body = "admin privileges required"};
        }
        std::string error;
        if (!reload_remote_state(state, logger, error)) {
            return RemoteResponse{.ok = false, .body = "reload failed: " + error};
        }
        return RemoteResponse{.ok = true, .body = "config reloaded"};
    }

    const RemoteStateSnapshot snapshot = snapshot_remote_state(state);
    const std::vector<std::string> mergedTokens = merged_command_arguments(commandTokens, snapshot.options.inheritedArguments);
    const ReqPackConfig effectiveConfig = apply_config_overrides(snapshot.config, extract_cli_config_overrides(mergedTokens));
    const std::vector<Request> requests = cli.parse(mergedTokens, effectiveConfig);
    if (requests.empty()) {
        return RemoteResponse{.ok = false, .body = "failed to parse '" + trimmed + "'"};
    }

    for (const Request& request : requests) {
        if (request.action == ActionType::SERVE) {
            return RemoteResponse{.ok = false, .body = "nested serve commands are not allowed"};
        }
    }

    if (snapshot.options.readonly && !requests_allowed_in_readonly_mode(requests)) {
        return RemoteResponse{.ok = false, .body = "remote server is readonly"};
    }

    std::lock_guard<std::mutex> lock(commandMutex);
    logger.flushSync();
    DisplayGuard displayGuard(logger, display);
    StdIoCapture capture;
    Orchestrator orchestrator(requests, effectiveConfig);
    const int result = orchestrator.run();
    logger.flushSync();
    return RemoteResponse{.ok = result == 0, .body = capture.finish()};
}

RemoteResponse execute_upload_install_command(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    const SessionIdentity& identity,
    const std::vector<std::string>& commandTokens,
    std::mutex& commandMutex
) {
    const std::optional<UploadInstallEnvelope> envelope = parse_upload_install_envelope(commandTokens);
    if (!envelope.has_value()) {
        return RemoteResponse{.ok = false, .body = "invalid upload request", .closeConnection = true};
    }

    if (snapshot_remote_state(state).options.readonly) {
        if (!discard_bytes(clientFd, envelope->size)) {
            return RemoteResponse{.ok = false, .body = "failed to read upload payload", .closeConnection = true};
        }
        return RemoteResponse{.ok = false, .body = "remote server is readonly"};
    }

    try {
        const ScopedPathCleanup uploadedFile = write_uploaded_file_to_temp(clientFd, envelope.value());
        const std::string commandLine = substitute_upload_path(envelope->commandTemplate, uploadedFile.path());
        return execute_command(cli, state, logger, display, identity, commandLine, commandMutex);
    } catch (const std::exception& e) {
        return RemoteResponse{.ok = false, .body = e.what()};
    }
}

bool authenticate_text_command(
    RemoteServerState& state,
    int sessionId,
    const std::string& line,
    SessionIdentity& identity,
    RemoteResponse& response
) {
    if (identity.authenticated) {
        return true;
    }

    const RemoteStateSnapshot snapshot = snapshot_remote_state(state);
    if (!auth_required(snapshot)) {
        identity = SessionIdentity{.authenticated = true, .userId = "anonymous", .isAdmin = false, .authType = "none"};
        update_session_identity(state, sessionId, identity);
        return true;
    }

    const std::vector<std::string> tokens = tokenize_command_line(line);
    if (tokens.size() >= 3 && tokens[0] == "auth" && tokens[1] == "token") {
        std::optional<SessionIdentity> resolved;
        if (has_user_registry(snapshot)) {
            resolved = resolve_remote_user_by_token(snapshot, tokens[2]);
        } else {
            resolved = resolve_fallback_identity_by_token(snapshot, tokens[2]);
        }
        if (resolved.has_value()) {
            identity = resolved.value();
            update_session_identity(state, sessionId, identity);
            response = RemoteResponse{.ok = true, .body = {}};
            return false;
        }
        response = RemoteResponse{.ok = false, .body = "authentication failed"};
        return false;
    }

    if (tokens.size() >= 4 && tokens[0] == "auth" && tokens[1] == "basic") {
        std::optional<SessionIdentity> resolved;
        if (has_user_registry(snapshot)) {
            resolved = resolve_remote_user_by_basic(snapshot, tokens[2], tokens[3]);
        } else {
            resolved = resolve_fallback_identity_by_basic(snapshot, tokens[2], tokens[3]);
        }
        if (resolved.has_value()) {
            identity = resolved.value();
            update_session_identity(state, sessionId, identity);
            response = RemoteResponse{.ok = true, .body = {}};
            return false;
        }
        response = RemoteResponse{.ok = false, .body = "authentication failed"};
        return false;
    }

    response = RemoteResponse{.ok = false, .body = "authentication required"};
    return false;
}

bool authenticate_json_command(
    RemoteServerState& state,
    int sessionId,
    const JsonCommand& command,
    SessionIdentity& identity,
    RemoteResponse& response
) {
    if (identity.authenticated) {
        return true;
    }

    const RemoteStateSnapshot snapshot = snapshot_remote_state(state);
    if (!auth_required(snapshot)) {
        identity = SessionIdentity{.authenticated = true, .userId = "anonymous", .isAdmin = false, .authType = "none"};
        update_session_identity(state, sessionId, identity);
        return true;
    }

    if (command.token.has_value()) {
        std::optional<SessionIdentity> resolved;
        if (has_user_registry(snapshot)) {
            resolved = resolve_remote_user_by_token(snapshot, command.token.value());
        } else {
            resolved = resolve_fallback_identity_by_token(snapshot, command.token.value());
        }
        if (resolved.has_value()) {
            identity = resolved.value();
            update_session_identity(state, sessionId, identity);
            return true;
        }
        response = RemoteResponse{.ok = false, .body = "authentication failed"};
        return false;
    }

    if (command.username.has_value() && command.password.has_value()) {
        std::optional<SessionIdentity> resolved;
        if (has_user_registry(snapshot)) {
            resolved = resolve_remote_user_by_basic(snapshot, command.username.value(), command.password.value());
        } else {
            resolved = resolve_fallback_identity_by_basic(snapshot, command.username.value(), command.password.value());
        }
        if (resolved.has_value()) {
            identity = resolved.value();
            update_session_identity(state, sessionId, identity);
            return true;
        }
        response = RemoteResponse{.ok = false, .body = "authentication failed"};
        return false;
    }

    response = RemoteResponse{.ok = false, .body = "authentication required"};
    return false;
}

void handle_text_client(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    int sessionId,
    std::mutex& commandMutex,
    std::optional<std::string> pendingLine = std::nullopt
) {
    SessionIdentity identity;
    if (!auth_required(snapshot_remote_state(state))) {
        identity = SessionIdentity{.authenticated = true, .userId = "anonymous", .isAdmin = false, .authType = "none"};
        update_session_identity(state, sessionId, identity);
    }

    for (;;) {
        std::optional<std::string> line;
        if (pendingLine.has_value()) {
            line = pendingLine;
            pendingLine.reset();
        } else {
            line = read_line_from_socket(clientFd);
        }
        if (!line.has_value()) {
            return;
        }

        RemoteResponse response;
        if (!authenticate_text_command(state, sessionId, line.value(), identity, response)) {
            const std::vector<std::string> authTokens = tokenize_command_line(trim_copy(line.value()));
            if (!authTokens.empty() && authTokens[0] == REMOTE_UPLOAD_INSTALL_COMMAND) {
                response.closeConnection = true;
            }
            if (!send_all(clientFd, text_response(response.ok, response.body))) {
                return;
            }
            if (response.closeConnection) {
                return;
            }
            continue;
        }

        const std::vector<std::string> commandTokens = tokenize_command_line(trim_copy(line.value()));
        if (!commandTokens.empty() && commandTokens[0] == REMOTE_UPLOAD_INSTALL_COMMAND) {
            response = execute_upload_install_command(
                clientFd,
                cli,
                state,
                logger,
                display,
                identity,
                commandTokens,
                commandMutex
            );
        } else {
            response = execute_command(cli, state, logger, display, identity, line.value(), commandMutex);
        }
        if (!send_all(clientFd, text_response(response.ok, response.body))) {
            return;
        }
        if (response.closeConnection) {
            return;
        }
    }
}

void handle_json_client(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    int sessionId,
    std::mutex& commandMutex,
    std::optional<std::string> pendingLine = std::nullopt
) {
    SessionIdentity identity;
    if (!auth_required(snapshot_remote_state(state))) {
        identity = SessionIdentity{.authenticated = true, .userId = "anonymous", .isAdmin = false, .authType = "none"};
        update_session_identity(state, sessionId, identity);
    }

    for (;;) {
        std::optional<std::string> line;
        if (pendingLine.has_value()) {
            line = pendingLine;
            pendingLine.reset();
        } else {
            line = read_line_from_socket(clientFd);
        }
        if (!line.has_value()) {
            return;
        }

        const std::optional<JsonCommand> request = parse_json_command(line.value());
        if (!request.has_value()) {
            if (!send_all(clientFd, json_response(false, "invalid json request"))) {
                return;
            }
            continue;
        }

        RemoteResponse response;
        if (!authenticate_json_command(state, sessionId, request.value(), identity, response)) {
            if (!send_all(clientFd, json_response(false, response.body))) {
                return;
            }
            continue;
        }

        if (request->command.empty()) {
            if (!send_all(clientFd, json_response(true, {}))) {
                return;
            }
            continue;
        }

        response = execute_command(cli, state, logger, display, identity, request->command, commandMutex);
        if (!send_all(clientFd, json_response(response.ok, response.body))) {
            return;
        }
        if (response.closeConnection) {
            return;
        }
    }
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

bool remote_protocol_requires_explicit_mode(ServeRemoteProtocol protocol) {
    return protocol == ServeRemoteProtocol::HTTP || protocol == ServeRemoteProtocol::HTTPS;
}

}  // namespace

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

    logger.info("remote server listening on " + options.bind + ":" + std::to_string(options.port));
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

    while (!state.shutdownRequested.load()) {
        sockaddr_storage clientAddress{};
        socklen_t clientLength = sizeof(clientAddress);
        const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        if (clientFd == -1) {
            if (state.shutdownRequested.load()) {
                break;
            }
            continue;
        }

        const RemoteStateSnapshot snapshot = snapshot_remote_state(state);
        if (active_connection_count(state) >= snapshot.options.maxConnections) {
            const std::string response = snapshot.options.remoteProtocol == ServeRemoteProtocol::JSON
                ? json_response(false, "max connections reached")
                : text_response(false, "max connections reached");
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
                    (void)send_all(clientFd, text_response(false, "unsupported negotiated protocol"));
                }
            }
            ::close(clientFd);
            std::lock_guard<std::mutex> lock(state.mutex);
            state.sessions.erase(sessionId);
        }).detach();
    }

    logger.flushSync();
    return 0;
}
