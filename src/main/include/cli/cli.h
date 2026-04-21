#pragma once

#include <CLI/CLI.hpp>

#include <memory>
#include <map>
#include <string>
#include <vector>

class Cli {

public:
    std::string command;
    std::vector<std::string> packages;
    std::map<std::string, bool> flags;


    Cli();
    void parse(int argc, char* argv[]);

    void print_help();

private:
    std::unique_ptr<CLI::App> app;
};
