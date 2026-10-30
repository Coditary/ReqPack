#include <catch2/catch.hpp>

#include "cli/cli_command_completion.h"

TEST_CASE("subcommand completion lists top-level commands", "[unit][cli][completion]") {
    const std::vector<std::string> completions = cli_internal::list_subcommand_completions("");
    REQUIRE(std::find(completions.begin(), completions.end(), "install") != completions.end());
    REQUIRE(std::find(completions.begin(), completions.end(), "plugin") != completions.end());
    REQUIRE(std::find(completions.begin(), completions.end(), "self-update") != completions.end());
}

TEST_CASE("subcommand completion expands plugin cluster", "[unit][cli][completion]") {
    const std::vector<std::string> completions = cli_internal::list_subcommand_completions("plugin.");
    REQUIRE(std::find(completions.begin(), completions.end(), "plugin.update") != completions.end());
    REQUIRE(std::find(completions.begin(), completions.end(), "plugin.info") != completions.end());
}

TEST_CASE("subcommand completion partial prefix matches security doctor", "[unit][cli][completion]") {
    const std::vector<std::string> completions = cli_internal::list_subcommand_completions("security.do");
    REQUIRE(completions.size() == 1);
    CHECK(completions.front() == "security.doctor");
}

TEST_CASE("global flag completion filters by prefix", "[unit][cli][completion]") {
    const std::vector<std::string> completions = cli_internal::list_global_flag_completions("--j");
    REQUIRE(std::find(completions.begin(), completions.end(), "--jobs") != completions.end());
    REQUIRE(std::find(completions.begin(), completions.end(), "--jobs-max") != completions.end());
}
