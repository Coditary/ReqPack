#include "serve_remote_internal.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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

ScopedPathCleanup::ScopedPathCleanup(std::filesystem::path path)
    : path_(std::move(path)) {
}

ScopedPathCleanup::~ScopedPathCleanup() {
    reset();
}

ScopedPathCleanup::ScopedPathCleanup(ScopedPathCleanup&& other) noexcept
    : path_(std::move(other.path_)) {
    other.path_.clear();
}

ScopedPathCleanup& ScopedPathCleanup::operator=(ScopedPathCleanup&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    path_ = std::move(other.path_);
    other.path_.clear();
    return *this;
}

const std::filesystem::path& ScopedPathCleanup::path() const {
    return path_;
}

void ScopedPathCleanup::reset() {
    if (path_.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::remove(path_, error);
    path_.clear();
}

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
