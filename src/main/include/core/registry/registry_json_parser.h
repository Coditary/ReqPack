#pragma once

#include "core/registry/registry_database.h"

#include <filesystem>
#include <vector>

struct RegistryJsonParseResult {
    std::vector<RegistryRecord> records;
};

RegistryJsonParseResult parse_registry_json_file(const std::filesystem::path& path);
