#include "serve_remote_internal.h"

namespace {

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

}  // namespace

bool has_user_registry(const RemoteStateSnapshot& snapshot) {
    return !snapshot.users.empty();
}

bool auth_required(const RemoteStateSnapshot& snapshot) {
    return has_user_registry(snapshot) || snapshot.options.token.has_value() ||
        (snapshot.options.username.has_value() && snapshot.options.password.has_value());
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
            response = RemoteResponse{.ok = true, .output = command_output_message(DisplayMode::REMOTE, {})};
            return false;
        }
        response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication failed", false)};
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
            response = RemoteResponse{.ok = true, .output = command_output_message(DisplayMode::REMOTE, {})};
            return false;
        }
        response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication failed", false)};
        return false;
    }

    response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication required", false)};
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
        response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication failed", false)};
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
        response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication failed", false)};
        return false;
    }

    response = RemoteResponse{.ok = false, .output = command_output_message(DisplayMode::REMOTE, "authentication required", false)};
    return false;
}
