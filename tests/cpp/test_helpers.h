#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#endif

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

inline void set_test_environment_value(const std::string& name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value.has_value() ? value->c_str() : "");
#else
    if (value.has_value()) {
        ::setenv(name.c_str(), value->c_str(), 1);
    } else {
        ::unsetenv(name.c_str());
    }
#endif
}

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(std::string name)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            previous_ = std::string(existing);
        }
    }

    ScopedEnvVar(std::string name, std::string value)
        : ScopedEnvVar(std::move(name)) {
        set_test_environment_value(name_, value);
    }

    ScopedEnvVar(const char* name, const char* value)
        : name_(name) {
        if (const char* existing = std::getenv(name_.c_str())) {
            previous_ = std::string(existing);
        }
        if (value != nullptr) {
            set_test_environment_value(name_, std::string(value));
        } else {
            set_test_environment_value(name_, std::nullopt);
        }
    }

    ~ScopedEnvVar() {
        if (previous_.has_value()) {
            set_test_environment_value(name_, previous_);
        } else {
            set_test_environment_value(name_, std::nullopt);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};
