#pragma once

#include <string>
#include <vector>
#include <map>

struct CliOutput {
    std::string command;
    std::vector<std::string> packages;
    std::map<std::string, bool> flags;
};

class Cli {
public:
    Cli();
    CliOutput parse(int argc, char* argv[]);
    
    void print_help();
};

