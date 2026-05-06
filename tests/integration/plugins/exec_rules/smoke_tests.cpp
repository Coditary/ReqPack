#include <filesystem>
#include <string>

#include <catch2/catch.hpp>

#include "test_helpers.h"

namespace {

std::string run_reqpack_case(const std::string& prompt) {
    const std::filesystem::path binary = build_root() / "rqp";
    const std::filesystem::path pluginDir = repo_root() / "tests" / "plugins";

    const std::string inner =
        escape_shell_arg(binary.string()) +
        " search smoke " + escape_shell_arg(prompt) +
        " --plugin-dir " + escape_shell_arg(pluginDir.string()) +
        " 2>&1";

    const std::string command = "sh -lc " + escape_shell_arg(inner);

    return run_command_capture(command);
}

std::string run_reqpack_case_verbose(const std::string& prompt) {
    const std::filesystem::path binary = build_root() / "rqp";
    const std::filesystem::path pluginDir = repo_root() / "tests" / "plugins";

    const std::string inner =
        escape_shell_arg(binary.string()) +
        " search smoke " + escape_shell_arg(prompt) +
        " --verbose" +
        " --plugin-dir " + escape_shell_arg(pluginDir.string()) +
        " 2>&1";

    const std::string command = "sh -lc " + escape_shell_arg(inner);

    return run_command_capture(command);
}

}  // namespace

TEST_CASE("plain exec path remains available", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("plain");
    CHECK(output.find("plain-ok") != std::string::npos);
    CHECK(output.find("plain fallback") != std::string::npos);
    CHECK(output.find("[smoke] (exec) plain fallback") == std::string::npos);
}

TEST_CASE("line rules emit progress and events", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("line");
    CHECK(output.find("[smoke]  progress 7%") != std::string::npos);
    CHECK(output.find("[smoke] (exec) Progress: 7%") == std::string::npos);
    CHECK(output.find("line_progress: 7") != std::string::npos);
    CHECK(output.find("[smoke]  progress 42%") != std::string::npos);
    CHECK(output.find("[smoke] (exec) Progress: 42%") == std::string::npos);
    CHECK(output.find("line_progress: 42") != std::string::npos);
    CHECK(output.find("line-ok") != std::string::npos);
}

TEST_CASE("screen rules perform send and stateful follow-up matching", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("screen");
    CHECK(output.find("sent confirm") == std::string::npos);
    CHECK(output.find("ack: y") != std::string::npos);
    CHECK(output.find("ack y") != std::string::npos);
    CHECK(output.find("[smoke]  progress 99%") != std::string::npos);
    CHECK(output.find("[smoke] (exec) Progress: 99%") == std::string::npos);
    CHECK(output.find("done") != std::string::npos);
    CHECK(output.find("screen-ok") != std::string::npos);
}

TEST_CASE("rich progress rules emit humanized metrics", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("rich");
    CHECK(output.find("[smoke]  progress 41%  16.4 MiB / 40.0 MiB  2.5 MiB/s") != std::string::npos);
    CHECK(output.find("[smoke] (exec) loaded:16.4/40.0@2.5") == std::string::npos);
    CHECK(output.find("line_progress: 16.4/40.0@2.5") != std::string::npos);
    CHECK(output.find("rich-ok") != std::string::npos);
}

TEST_CASE("repeat and stop alter follow-up rule execution", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("control");
    CHECK(output.find("first: one") != std::string::npos);
    CHECK(output.find("control-ok") != std::string::npos);
    CHECK(output.find("second: two") == std::string::npos);
}

TEST_CASE("invalid rules fail before process execution", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case("invalid");
    CHECK(output.find("exec-rule error: rules.rules[1].source must be 'line' or 'screen'.") != std::string::npos);
    CHECK(output.find("invalid-ok") != std::string::npos);
}

TEST_CASE("verbose mode shows exec transcript in terminal output", "[integration][exec_rules][smoke]") {
    const std::string output = run_reqpack_case_verbose("screen");
    CHECK(output.find("Continue? [y/N]:") != std::string::npos);
    CHECK(output.find("ACK=y") != std::string::npos);
    CHECK(output.find("Progress: 99%") != std::string::npos);
    CHECK(output.find("progress 99%") != std::string::npos);
}
