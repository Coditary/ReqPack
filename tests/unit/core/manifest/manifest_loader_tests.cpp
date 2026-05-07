#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "core/manifest/manifest_loader.h"

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() /
              (prefix + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path manifest() const { return path_ / MANIFEST_FILENAME; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << content;
}

}  // namespace

// ---------------------------------------------------------------------------
// Happy-path: return-table form
// ---------------------------------------------------------------------------

TEST_CASE("manifest loads packages from returned table", "[unit][manifest][load]") {
    TempDir dir{"reqpack-manifest-return-table"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf",  name = "curl" },
                { system = "dnf",  name = "git",     version = "2.40.0" },
                { system = "npm",  name = "express",  version = "4.18.0" },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 3);

    CHECK(entries[0].system  == "dnf");
    CHECK(entries[0].name    == "curl");
    CHECK(entries[0].version == "");

    CHECK(entries[1].system  == "dnf");
    CHECK(entries[1].name    == "git");
    CHECK(entries[1].version == "2.40.0");

    CHECK(entries[2].system  == "npm");
    CHECK(entries[2].name    == "express");
    CHECK(entries[2].version == "4.18.0");
}

// ---------------------------------------------------------------------------
// Happy-path: global variable form
// ---------------------------------------------------------------------------

TEST_CASE("manifest loads packages from global variable", "[unit][manifest][load]") {
    TempDir dir{"reqpack-manifest-global-var"};
    write_file(dir.manifest(), R"(
        packages = {
            { system = "apt", name = "htop" },
            { system = "apt", name = "wget", version = "1.21" },
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 2);

    CHECK(entries[0].system  == "apt");
    CHECK(entries[0].name    == "htop");
    CHECK(entries[0].version == "");

    CHECK(entries[1].system  == "apt");
    CHECK(entries[1].name    == "wget");
    CHECK(entries[1].version == "1.21");
}

// ---------------------------------------------------------------------------
// Return-table form takes priority over global variable
// ---------------------------------------------------------------------------

TEST_CASE("manifest prefers returned table over global variable", "[unit][manifest][load]") {
    TempDir dir{"reqpack-manifest-priority"};
    write_file(dir.manifest(), R"(
        packages = {
            { system = "ignored", name = "should-not-appear" },
        }
        return {
            packages = {
                { system = "dnf", name = "curl" },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].system == "dnf");
    CHECK(entries[0].name   == "curl");
}

// ---------------------------------------------------------------------------
// Optional fields
// ---------------------------------------------------------------------------

TEST_CASE("manifest entry without version has empty version string", "[unit][manifest][fields]") {
    TempDir dir{"reqpack-manifest-no-version"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "brew", name = "jq" },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].version == "");
    CHECK(entries[0].flags.empty());
}

TEST_CASE("manifest entry with flags populates flags vector", "[unit][manifest][fields]") {
    TempDir dir{"reqpack-manifest-flags"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf", name = "curl", flags = { "no-docs", "minimal" } },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].flags.size() == 2);
    CHECK(entries[0].flags[0] == "no-docs");
    CHECK(entries[0].flags[1] == "minimal");
}

TEST_CASE("manifest with empty flags array produces empty flags vector", "[unit][manifest][fields]") {
    TempDir dir{"reqpack-manifest-empty-flags"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf", name = "curl", flags = {} },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].flags.empty());
}

// ---------------------------------------------------------------------------
// Empty packages table
// ---------------------------------------------------------------------------

TEST_CASE("manifest with empty packages table returns empty vector", "[unit][manifest][load]") {
    TempDir dir{"reqpack-manifest-empty"};
    write_file(dir.manifest(), R"(
        return {
            packages = {}
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    CHECK(entries.empty());
}

// ---------------------------------------------------------------------------
// Multiple packages, mixed systems
// ---------------------------------------------------------------------------

TEST_CASE("manifest preserves declaration order across mixed systems", "[unit][manifest][load]") {
    TempDir dir{"reqpack-manifest-mixed"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf",  name = "curl" },
                { system = "npm",  name = "express" },
                { system = "dnf",  name = "git" },
                { system = "brew", name = "jq" },
            }
        }
    )");

    const std::vector<ManifestEntry> entries = ManifestLoader::load(dir.manifest());
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].system == "dnf");   CHECK(entries[0].name == "curl");
    CHECK(entries[1].system == "npm");   CHECK(entries[1].name == "express");
    CHECK(entries[2].system == "dnf");   CHECK(entries[2].name == "git");
    CHECK(entries[3].system == "brew");  CHECK(entries[3].name == "jq");
}

// ---------------------------------------------------------------------------
// Error: file missing
// ---------------------------------------------------------------------------

TEST_CASE("manifest throws when file does not exist", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-missing"};
    const std::filesystem::path nonexistent = dir.path() / "reqpack.lua";

    CHECK_THROWS_AS(ManifestLoader::load(nonexistent), std::runtime_error);
}

TEST_CASE("manifest error message contains the missing path", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-missing-msg"};
    const std::filesystem::path nonexistent = dir.path() / "reqpack.lua";

    try {
        ManifestLoader::load(nonexistent);
        FAIL("expected exception");
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        CHECK(msg.find(nonexistent.string()) != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Error: invalid Lua syntax
// ---------------------------------------------------------------------------

TEST_CASE("manifest throws on invalid lua syntax", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-syntax"};
    write_file(dir.manifest(), "this is not valid lua @@@@");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Error: no packages table
// ---------------------------------------------------------------------------

TEST_CASE("manifest throws when packages table is absent", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-no-packages"};
    write_file(dir.manifest(), R"(
        return {
            dependencies = {
                { system = "dnf", name = "curl" },
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

TEST_CASE("manifest throws when file returns a non-table and no global packages", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-returns-string"};
    write_file(dir.manifest(), R"(return "oops")");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Error: missing required entry fields
// ---------------------------------------------------------------------------

TEST_CASE("manifest throws when entry is missing system field", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-no-system"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { name = "curl" },
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

TEST_CASE("manifest throws when entry has empty system field", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-empty-system"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "", name = "curl" },
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

TEST_CASE("manifest throws when entry is missing name field", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-no-name"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf" },
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

TEST_CASE("manifest throws when entry has empty name field", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-empty-name"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf", name = "" },
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Error: entry is not a table
// ---------------------------------------------------------------------------

TEST_CASE("manifest throws when a packages entry is not a table", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-non-table-entry"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                "not-a-table",
            }
        }
    )");

    CHECK_THROWS_AS(ManifestLoader::load(dir.manifest()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Error message contains entry index
// ---------------------------------------------------------------------------

TEST_CASE("manifest error message contains failing entry index", "[unit][manifest][error]") {
    TempDir dir{"reqpack-manifest-error-index"};
    write_file(dir.manifest(), R"(
        return {
            packages = {
                { system = "dnf", name = "curl" },
                { system = "dnf" },
            }
        }
    )");

    try {
        ManifestLoader::load(dir.manifest());
        FAIL("expected exception");
    } catch (const std::runtime_error& e) {
        const std::string msg(e.what());
        // Second entry (#2) is the broken one
        CHECK(msg.find("#2") != std::string::npos);
    }
}
