#pragma once

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef REQPACK_TEST_REPO_ROOT
#error "REQPACK_TEST_REPO_ROOT must be defined for test targets"
#endif

#ifndef REQPACK_TEST_BUILD_DIR
#error "REQPACK_TEST_BUILD_DIR must be defined for test targets"
#endif

inline std::string escape_shell_arg(const std::string& value) {
    std::string escaped{"'"};
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

inline std::string run_command_capture(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run command: " + command);
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    const int status = pclose(pipe);
    if (status == -1) {
        throw std::runtime_error("failed to close command pipe: " + command);
    }
    return output;
}

inline std::filesystem::path repo_root() {
    return std::filesystem::path(REQPACK_TEST_REPO_ROOT);
}

inline std::filesystem::path build_root() {
    return std::filesystem::path(REQPACK_TEST_BUILD_DIR);
}
