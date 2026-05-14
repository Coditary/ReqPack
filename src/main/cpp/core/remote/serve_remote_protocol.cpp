#include "serve_remote_internal.h"

#include <cctype>
#include <sstream>

namespace {

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

}  // namespace

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

std::string json_response(bool ok, const CommandOutput& output) {
    const std::string body = render_command_output_text(output);
    if (ok) {
        return std::string{"{"} + "\"ok\":true," + json_string_field("output", body) + "}\n";
    }
    return std::string{"{"} + "\"ok\":false," + json_string_field("error", body) + "}\n";
}

std::string text_response(bool ok, const CommandOutput& output) {
    const std::string body = render_command_output_text(output);
    return std::string(ok ? "OK " : "ERR ") + std::to_string(body.size()) + "\n" + body;
}

void handle_text_client(
    int clientFd,
    Cli& cli,
    RemoteServerState& state,
    Logger& logger,
    IDisplay* display,
    int sessionId,
    std::mutex& commandMutex,
    std::optional<std::string> pendingLine
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
            if (!send_all(clientFd, text_response(response.ok, response.output))) {
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
        if (!send_all(clientFd, text_response(response.ok, response.output))) {
            return;
        }
        if (response.closeConnection) {
            if (state.shutdownRequested.load()) {
                request_server_shutdown(state);
            }
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
    std::optional<std::string> pendingLine
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
            if (!send_all(clientFd, json_response(false, command_output_message(DisplayMode::REMOTE, "invalid json request", false)))) {
                return;
            }
            continue;
        }

        RemoteResponse response;
        if (!authenticate_json_command(state, sessionId, request.value(), identity, response)) {
            if (!send_all(clientFd, json_response(false, response.output))) {
                return;
            }
            continue;
        }

        if (request->command.empty()) {
            if (!send_all(clientFd, json_response(true, command_output_message(DisplayMode::REMOTE, {})))) {
                return;
            }
            continue;
        }

        response = execute_command(cli, state, logger, display, identity, request->command, commandMutex);
        if (!send_all(clientFd, json_response(response.ok, response.output))) {
            return;
        }
        if (response.closeConnection) {
            if (state.shutdownRequested.load()) {
                request_server_shutdown(state);
            }
            return;
        }
    }
}
