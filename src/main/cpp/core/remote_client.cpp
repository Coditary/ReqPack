#include "core/remote_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {

constexpr const char* REMOTE_UPLOAD_INSTALL_COMMAND = "__reqpack_upload_install__";
constexpr const char* REMOTE_UPLOAD_PATH_PLACEHOLDER = "__REQPACK_REMOTE_UPLOAD_PATH__";

struct UploadInstallRequest {
    std::filesystem::path filePath;
    std::string filename;
    std::string commandTemplate;
    std::uintmax_t size{0};
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

std::string join_arguments(const std::vector<std::string>& arguments) {
    std::string command;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i > 0) {
            command += ' ';
        }
        const std::string& argument = arguments[i];
        const bool needsQuotes = argument.find_first_of(" \t\r\n\"'") != std::string::npos;
        if (!needsQuotes) {
            command += argument;
            continue;
        }
        command.push_back('"');
        for (char c : argument) {
            if (c == '\\' || c == '"') {
                command.push_back('\\');
            }
            command.push_back(c);
        }
        command.push_back('"');
    }
    return command;
}

std::optional<UploadInstallRequest> detect_upload_install_request(
    const std::vector<std::string>& arguments,
    std::string& error
) {
    error.clear();
    if (arguments.empty() || arguments.front() != "install") {
        return std::nullopt;
    }

    std::vector<std::size_t> positionalIndices;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignoredOverrides;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignoredOverrides)) {
            i = configIndex;
            continue;
        }
        if (arguments[i].rfind("--", 0) == 0) {
            continue;
        }
        positionalIndices.push_back(i);
    }

    if (positionalIndices.size() < 2) {
        return std::nullopt;
    }

    const std::filesystem::path localPath = arguments[positionalIndices[1]];
    std::error_code fsError;
    if (!std::filesystem::exists(localPath, fsError) || fsError) {
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(localPath, fsError) || fsError) {
        error = "remote upload only supports regular files";
        return std::nullopt;
    }
    if (positionalIndices.size() > 2) {
        error = "remote upload supports one local file per install command";
        return std::nullopt;
    }

    UploadInstallRequest request;
    request.filePath = localPath;
    request.filename = localPath.filename().string();
    request.size = std::filesystem::file_size(localPath, fsError);
    if (fsError) {
        error = "failed to inspect local upload file";
        return std::nullopt;
    }

    std::vector<std::string> rewrittenArguments = arguments;
    rewrittenArguments[positionalIndices[1]] = REMOTE_UPLOAD_PATH_PLACEHOLDER;
    request.commandTemplate = join_arguments(rewrittenArguments);
    return request;
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

std::optional<std::string> read_line(int fd) {
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

std::string read_bytes(int fd, std::size_t count) {
    std::string out(count, '\0');
    std::size_t offset = 0;
    while (offset < count) {
        const ssize_t received = ::recv(fd, out.data() + offset, count - offset, 0);
        if (received <= 0) {
            throw std::runtime_error("failed to read remote response body");
        }
        offset += static_cast<std::size_t>(received);
    }
    return out;
}

std::pair<std::string, std::string> read_text_response(int fd) {
    const std::optional<std::string> header = read_line(fd);
    if (!header.has_value()) {
        throw std::runtime_error("remote server closed connection");
    }
    const std::size_t separator = header->find(' ');
    if (separator == std::string::npos) {
        throw std::runtime_error("invalid remote response header");
    }
    const std::string status = header->substr(0, separator);
    const std::size_t length = static_cast<std::size_t>(std::stoul(header->substr(separator + 1)));
    return {status, read_bytes(fd, length)};
}

std::string escape_json(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
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

int connect_remote(const RemoteProfile& profile) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(profile.port);
    if (::getaddrinfo(profile.host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        throw std::runtime_error("invalid remote host: " + profile.host);
    }

    int fd = -1;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(addresses);
    if (fd == -1) {
        throw std::runtime_error("failed to connect to remote profile '" + profile.name + "'");
    }
    return fd;
}

int send_text_command_and_print_response(int fd, const std::string& command) {
    if (!send_all(fd, command + "\n")) {
        throw std::runtime_error("failed to send remote command");
    }
    const auto response = read_text_response(fd);
    std::cout << response.second;
    return response.first == "OK" ? 0 : 1;
}

int send_upload_install_request(int fd, const UploadInstallRequest& request) {
    const std::string header = join_arguments({
        REMOTE_UPLOAD_INSTALL_COMMAND,
        std::to_string(request.size),
        request.filename,
        request.commandTemplate,
    });
    if (!send_all(fd, header + "\n")) {
        throw std::runtime_error("failed to send remote upload header");
    }

    std::ifstream input(request.filePath, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open local upload file: " + request.filePath.string());
    }

    char buffer[8192];
    while (input.good()) {
        input.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            break;
        }
        if (!send_all(fd, std::string(buffer, static_cast<std::size_t>(count)))) {
            throw std::runtime_error("failed to stream local upload file");
        }
    }
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("failed while reading local upload file");
    }

    const auto response = read_text_response(fd);
    std::cout << response.second;
    return response.first == "OK" ? 0 : 1;
}

int run_text_client_session(const RemoteProfile& profile, const std::vector<std::string>& forwardedArguments) {
    std::optional<UploadInstallRequest> forwardedUpload;
    if (!forwardedArguments.empty()) {
        std::string uploadError;
        forwardedUpload = detect_upload_install_request(forwardedArguments, uploadError);
        if (!uploadError.empty()) {
            throw std::runtime_error(uploadError);
        }
    }

    const int fd = connect_remote(profile);
    auto closeGuard = [&]() { ::close(fd); };

    if (profile.token.has_value()) {
        if (!send_all(fd, "auth token " + profile.token.value() + "\n")) {
            closeGuard();
            throw std::runtime_error("failed to send token auth request");
        }
        const auto authResponse = read_text_response(fd);
        if (authResponse.first != "OK") {
            closeGuard();
            throw std::runtime_error(authResponse.second.empty() ? "authentication failed" : authResponse.second);
        }
    } else if (profile.username.has_value() && profile.password.has_value()) {
        if (!send_all(fd, "auth basic " + profile.username.value() + " " + profile.password.value() + "\n")) {
            closeGuard();
            throw std::runtime_error("failed to send basic auth request");
        }
        const auto authResponse = read_text_response(fd);
        if (authResponse.first != "OK") {
            closeGuard();
            throw std::runtime_error(authResponse.second.empty() ? "authentication failed" : authResponse.second);
        }
    }

    if (!forwardedArguments.empty()) {
        const int result = forwardedUpload.has_value()
            ? send_upload_install_request(fd, forwardedUpload.value())
            : send_text_command_and_print_response(fd, join_arguments(forwardedArguments));
        closeGuard();
        return result;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty()) {
            continue;
        }
        const std::vector<std::string> tokens = tokenize_command_line(trimmed);
        if (tokens.empty()) {
            std::cerr << "invalid command syntax\n";
            continue;
        }

        std::string uploadError;
        const std::optional<UploadInstallRequest> upload = detect_upload_install_request(tokens, uploadError);
        if (!uploadError.empty()) {
            std::cerr << uploadError << '\n';
            continue;
        }

        const int result = upload.has_value()
            ? send_upload_install_request(fd, upload.value())
            : send_text_command_and_print_response(fd, trimmed);
        if (trimmed == "quit" || trimmed == "exit") {
            break;
        }
        (void)result;
    }

    closeGuard();
    return 0;
}

int run_json_client_session(const RemoteProfile& profile, const std::vector<std::string>& forwardedArguments) {
    if (forwardedArguments.empty()) {
        throw std::runtime_error("json remote profiles require a forwarded command");
    }

    std::string uploadError;
    if (detect_upload_install_request(forwardedArguments, uploadError).has_value() || !uploadError.empty()) {
        if (uploadError.empty()) {
            uploadError = "file upload requires text protocol";
        }
        throw std::runtime_error(uploadError == "remote upload only supports regular files" ||
                uploadError == "remote upload supports one local file per install command"
            ? uploadError
            : "file upload requires text protocol");
    }

    const int fd = connect_remote(profile);
    auto closeGuard = [&]() { ::close(fd); };

    std::string request = "{" + json_string_field("command", join_arguments(forwardedArguments));
    if (profile.token.has_value()) {
        request += "," + json_string_field("token", profile.token.value());
    }
    if (profile.username.has_value()) {
        request += "," + json_string_field("username", profile.username.value());
    }
    if (profile.password.has_value()) {
        request += "," + json_string_field("password", profile.password.value());
    }
    request += "}\n";

    if (!send_all(fd, request)) {
        closeGuard();
        throw std::runtime_error("failed to send remote json request");
    }

    const std::optional<std::string> responseLine = read_line(fd);
    if (!responseLine.has_value()) {
        closeGuard();
        throw std::runtime_error("remote server closed json connection");
    }
    const std::optional<std::string> output = extract_json_string_field(responseLine.value(), "output");
    const std::optional<std::string> error = extract_json_string_field(responseLine.value(), "error");
    if (output.has_value()) {
        std::cout << output.value();
        closeGuard();
        return 0;
    }
    closeGuard();
    throw std::runtime_error(error.value_or("remote json request failed"));
}

}  // namespace

int run_remote_client(
    const ReqPackConfig& config,
    const std::filesystem::path& profilePath,
    const std::string& profileName,
    const std::vector<std::string>& forwardedArguments
) {
    (void)config;
    const std::optional<RemoteProfile> profile = find_remote_profile(profilePath, profileName);
    if (!profile.has_value()) {
        throw std::runtime_error("remote profile not found: " + profileName);
    }

    switch (profile->protocol) {
        case RemoteProfileProtocol::JSON:
            return run_json_client_session(profile.value(), forwardedArguments);
        case RemoteProfileProtocol::HTTP:
            throw std::runtime_error("http remote profiles are not implemented yet");
        case RemoteProfileProtocol::HTTPS:
            throw std::runtime_error("https remote profiles are not implemented yet");
        case RemoteProfileProtocol::AUTO:
        case RemoteProfileProtocol::TEXT:
        default:
            return run_text_client_session(profile.value(), forwardedArguments);
    }
}
