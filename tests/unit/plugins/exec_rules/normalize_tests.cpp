#include <string>

#include <catch2/catch.hpp>

#include "plugins/exec_rules_core.h"

TEST_CASE("PTY normalization removes ANSI sequences", "[unit][exec_rules][normalize]") {
    const std::string input = "\x1b[31mHello\x1b[0m";
    CHECK(normalize_exec_rule_pty_chunk(input) == "Hello");
}

TEST_CASE("PTY normalization turns carriage return into newline", "[unit][exec_rules][normalize]") {
    const std::string input = "Downloading 1%\rDownloading 2%\n";
    CHECK(normalize_exec_rule_pty_chunk(input) == "Downloading 1%\nDownloading 2%\n");
}

TEST_CASE("PTY normalization applies backspace", "[unit][exec_rules][normalize]") {
    const std::string input = "ab\bc\n";
    CHECK(normalize_exec_rule_pty_chunk(input) == "ac\n");
}
