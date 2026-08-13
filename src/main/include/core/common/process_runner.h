#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ReqpackProcessResult {
    int exitCode{1};
    std::string stdoutText;
    std::string stderrText;

    bool success() const {
        return exitCode == 0;
    }
};

bool reqpack_run_process(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory = {});

ReqpackProcessResult reqpack_run_process_capture(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory = {});
