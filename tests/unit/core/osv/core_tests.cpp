#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/osv_core.h"

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

TEST_CASE("osv parser reads advisory fields and matches package versions", "[unit][osv][core]") {
    const std::string json = R"({
        "id": "GHSA-1234",
        "modified": "2026-01-01T00:00:00Z",
        "published": "2025-12-31T00:00:00Z",
        "aliases": ["CVE-2026-1"],
        "summary": "demo summary",
        "severity": [{"type": "CVSS_V3", "score": "9.8"}],
        "affected": [{
            "package": {
                "ecosystem": "npm",
                "name": "react"
            },
            "versions": ["18.3.1"],
            "ranges": [{
                "type": "SEMVER",
                "events": [
                    {"introduced": "0"},
                    {"fixed": "19.0.0"}
                ]
            }]
        }]
    })";

    const auto advisory = osv_parse_advisory(json);
    REQUIRE(advisory.has_value());
    CHECK(advisory->id == "GHSA-1234");
    CHECK(advisory->aliases == std::vector<std::string>{"CVE-2026-1"});
    CHECK(advisory->severity == "critical");
    REQUIRE(advisory->affected.size() == 1);
    CHECK(advisory->affected[0].ecosystem == "npm");
    CHECK(advisory->affected[0].name == "react");

    const auto exactMatch = osv_match_package(advisory.value(), "npm", "react", "18.3.1");
    REQUIRE(exactMatch.has_value());
    CHECK(exactMatch->severity == "critical");
    CHECK(exactMatch->score == Approx(9.8));

    const auto miss = osv_match_package(advisory.value(), "npm", "react", "19.1.0");
    CHECK_FALSE(miss.has_value());
}

TEST_CASE("osv loader accepts top-level array files", "[unit][osv][core]") {
    TempDir tempDir{"reqpack-osv"};
    const std::filesystem::path path = tempDir.path() / "overlay.json";
    write_file(path, R"([
        {
            "id": "CVE-2026-1",
            "modified": "2026-01-01T00:00:00Z",
            "affected": [{"package": {"ecosystem": "npm", "name": "react"}}]
        },
        {
            "id": "CVE-2026-2",
            "modified": "2026-01-02T00:00:00Z",
            "affected": [{"package": {"ecosystem": "Maven", "name": "org.demo:lib"}}]
        }
    ])");

    const std::vector<OsvAdvisory> advisories = osv_load_advisories_from_path(path);
    REQUIRE(advisories.size() == 2);
    CHECK(advisories[0].id == "CVE-2026-1");
    CHECK(advisories[1].id == "CVE-2026-2");
}
