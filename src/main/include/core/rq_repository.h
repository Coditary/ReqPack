#pragma once

#include <optional>
#include <string>
#include <vector>

struct RqRepositoryPackage {
    std::string repository;
    std::string name;
    std::string version;
    int release{0};
    int revision{0};
    std::string architecture;
    std::string summary;
    std::string url;
    std::string packageSha256;
    std::vector<std::string> tags;
};

struct RqRepositoryIndex {
    std::string source;
    std::vector<RqRepositoryPackage> packages;
};

RqRepositoryIndex rq_repository_parse_index(const std::string& content, const std::string& source);

std::optional<RqRepositoryPackage> rq_repository_resolve_package(
    const std::vector<RqRepositoryIndex>& indexes,
    const std::string& name,
    const std::string& version,
    const std::string& hostArchitecture
);
