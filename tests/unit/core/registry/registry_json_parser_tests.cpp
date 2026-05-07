#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "core/registry/registry_json_parser.h"

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() /
            (prefix + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

}  // namespace

TEST_CASE("registry json parser builds main and alias records", "[unit][registry_json_parser][core]") {
    TempDir tempDir{"reqpack-registry-json-parser"};
    const std::filesystem::path path = tempDir.path() / "registry" / "d" / "dnf.json";
    write_file(path, R"({
  "schemaVersion": 1,
  "name": "dnf",
  "source": "git+https://github.com/Coditary/reqpack-dnf-plugin.git@v2.0.0",
  "description": "Fedora DNF",
  "role": "Package-Manager",
  "capabilities": ["EXEC", "network"],
  "ecosystemScopes": ["RPM", "fedora"],
  "writeScopes": [
    { "kind": "Temp" },
    { "kind": "user-home-subpath", "value": ".cache/dnf" }
  ],
  "networkScopes": [
    { "host": "API.OSV.DEV", "scheme": "HTTPS", "pathPrefix": "/v1" }
  ],
  "privilegeLevel": "SUDO",
  "scriptSha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "bootstrapSha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "aliases": [
    { "name": "yum", "description": "Alias for dnf" },
    "fedora-dnf"
  ]
})");

    const RegistryJsonParseResult parsed = parse_registry_json_file(path);
    REQUIRE(parsed.records.size() == 3);

    const RegistryRecord& main = parsed.records[0];
    CHECK(main.name == "dnf");
    CHECK_FALSE(main.alias);
    CHECK(main.originPath == path.generic_string());
    CHECK(main.role == "package-manager");
    CHECK(main.capabilities == std::vector<std::string>{"exec", "network"});
    CHECK(main.ecosystemScopes == std::vector<std::string>{"rpm", "fedora"});
    REQUIRE(main.writeScopes.size() == 2);
    CHECK(main.writeScopes[0].kind == "temp");
    CHECK(main.writeScopes[1].value == ".cache/dnf");
    REQUIRE(main.networkScopes.size() == 1);
    CHECK(main.networkScopes[0].host == "api.osv.dev");
    CHECK(main.networkScopes[0].scheme == "https");
    CHECK(main.privilegeLevel == "sudo");

    CHECK(parsed.records[1].alias);
    CHECK(parsed.records[1].name == "yum");
    CHECK(parsed.records[1].source == "dnf");
    CHECK(parsed.records[1].description == "Alias for dnf");
    CHECK(parsed.records[1].originPath == path.generic_string());

    CHECK(parsed.records[2].alias);
    CHECK(parsed.records[2].name == "fedora-dnf");
    CHECK(parsed.records[2].source == "dnf");
}

TEST_CASE("registry json parser rejects mismatched file and missing fields", "[unit][registry_json_parser][core]") {
    TempDir tempDir{"reqpack-registry-json-parser-invalid"};
    const std::filesystem::path path = tempDir.path() / "registry" / "m" / "maven.json";

    write_file(path, R"({
  "schemaVersion": 1,
  "name": "dnf",
  "source": "git+https://github.com/Coditary/reqpack-maven-plugin.git@v2.0.0"
})");
    CHECK_THROWS(parse_registry_json_file(path));

    write_file(path, R"({
  "schemaVersion": 1,
  "name": "maven"
})");
    CHECK_THROWS(parse_registry_json_file(path));
}
