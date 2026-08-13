#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

inline std::filesystem::path reqpack_make_unique_directory(
    const std::filesystem::path& parent,
    const std::string& prefix
) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        throw std::runtime_error("failed to create directory: " + parent.string());
    }

#if !defined(_WIN32)
    const std::filesystem::path pattern = parent / (prefix + "-XXXXXX");
    std::string templateString = pattern.string();
    std::vector<char> buffer(templateString.begin(), templateString.end());
    buffer.push_back('\0');
    if (char* created = ::mkdtemp(buffer.data()); created != nullptr) {
        return std::filesystem::path(created);
    }
#endif

    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::filesystem::path candidate = parent / (prefix + "-" + std::to_string(attempt));
        error.clear();
        if (std::filesystem::create_directory(candidate, error) && !error) {
            return candidate;
        }
    }

    throw std::runtime_error("failed to create unique directory under: " + parent.string());
}
