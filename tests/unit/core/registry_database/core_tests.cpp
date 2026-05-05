#include <catch2/catch.hpp>

#include <filesystem>

#include "core/registry_database_core.h"

TEST_CASE("registry database validates plugin script payloads", "[unit][registry_database][payload]") {
    CHECK(registry_database_is_valid_plugin_script("return { getName = function() return 'x' end }") );
    CHECK_FALSE(registry_database_is_valid_plugin_script("   \n\t  "));
    CHECK_FALSE(registry_database_is_valid_plugin_script("<!DOCTYPE html><html><body>oops</body></html>"));
    CHECK_FALSE(registry_database_is_valid_plugin_script("<html><head></head><body>oops</body></html>"));
}

TEST_CASE("registry database identifies git and non-git sources", "[unit][registry_database][source]") {
    CHECK(registry_database_is_git_source("git+https://github.com/org/repo.git"));
    CHECK(registry_database_is_git_source("git@github.com:org/repo.git"));
    CHECK(registry_database_is_git_source("ssh://git@github.com/org/repo.git"));
    CHECK(registry_database_is_git_source("https://github.com/org/repo.git?ref=main"));
    CHECK_FALSE(registry_database_is_git_source("https://example.test/plugin.lua"));
    CHECK_FALSE(registry_database_is_git_source("/tmp/plugin.lua"));
}

TEST_CASE("registry database normalizes git source url and strips query fragments", "[unit][registry_database][source]") {
    CHECK(registry_database_git_source_url("git+https://github.com/org/repo.git") == "https://github.com/org/repo.git");
    CHECK(registry_database_git_source_url("https://github.com/org/repo.git") == "https://github.com/org/repo.git");
    CHECK(registry_database_strip_query_fragment("https://github.com/org/repo.git?ref=main#frag") == "https://github.com/org/repo.git");
    CHECK(registry_database_git_source_ref("git+https://github.com/org/repo.git?ref=v1.2.3") == "v1.2.3");
    CHECK(registry_database_git_source_ref("https://github.com/org/repo.git#v2.0.0") == "v2.0.0");
    CHECK(registry_database_git_source_with_ref("git+https://github.com/org/repo.git", "v1.2.3") == "git+https://github.com/org/repo.git?ref=v1.2.3");
}

TEST_CASE("registry database extracts git tags from ls-remote output", "[unit][registry_database][source]") {
    const std::vector<std::string> tags = registry_database_extract_git_tags(
        "abc123\trefs/tags/v1.0.0\n"
        "def456\trefs/tags/v1.1.0\n"
    );

    REQUIRE(tags.size() == 2);
    CHECK(tags[0] == "v1.0.0");
    CHECK(tags[1] == "v1.1.0");
}

TEST_CASE("registry database cache path derivation is stable for source and plugin name", "[unit][registry_database][source]") {
    ReqPackConfig config;
    config.registry.databasePath = "/tmp/reqpack-registry";

    const std::filesystem::path first = registry_database_git_repository_cache_path(config, "git+https://github.com/org/repo.git", "dnf");
    const std::filesystem::path second = registry_database_git_repository_cache_path(config, "git+https://github.com/org/repo.git", "dnf");
    const std::filesystem::path differentPlugin = registry_database_git_repository_cache_path(config, "git+https://github.com/org/repo.git", "maven");
    const std::filesystem::path differentSource = registry_database_git_repository_cache_path(config, "git+https://github.com/org/other.git", "dnf");

    CHECK(first == second);
    CHECK(first.parent_path() == default_reqpack_repo_cache_path());
    CHECK(first.filename() != differentPlugin.filename());
    CHECK(first.filename() != differentSource.filename());
}

TEST_CASE("registry database record serialization round-trips escaped fields", "[unit][registry_database][serialization]") {
    RegistryRecord record;
    record.name = "dnf";
    record.source = "https://example.test/plugin.lua";
    record.alias = false;
    record.description = "line1\nline2";
    record.script = "return {}\n";
    record.bootstrapScript = "print('boot\\strap')\n";
    record.bundleSource = true;
    record.bundlePath = "/tmp/plugins\\dnf";

    const std::string payload = registry_database_serialize_record(record);
    const std::optional<RegistryRecord> parsed = registry_database_deserialize_record("dnf", payload);

    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "dnf");
    CHECK(parsed->source == record.source);
    CHECK_FALSE(parsed->alias);
    CHECK(parsed->description == record.description);
    CHECK(parsed->script == record.script);
    CHECK(parsed->bootstrapScript == record.bootstrapScript);
    CHECK(parsed->bundleSource);
    CHECK(parsed->bundlePath == record.bundlePath);
}

TEST_CASE("registry database deserializer rejects bad payload shapes", "[unit][registry_database][serialization]") {
    CHECK_FALSE(registry_database_deserialize_record(
        "dnf",
        "source=https://example.test/plugin.lua\nalias=0\ndescription=x\nbundleSource=0\nbundlePath=\nbootstrap=\nreturn {}"
    ).has_value());

    CHECK_FALSE(registry_database_deserialize_record(
        "dnf",
        "source=https://example.test/plugin.lua\nalias=maybe\ndescription=x\nbundleSource=0\nbundlePath=\nbootstrap=\n---\nreturn {}"
    ).has_value());

    CHECK_FALSE(registry_database_deserialize_record(
        "dnf",
        "source=\nalias=0\ndescription=x\nbundleSource=0\nbundlePath=\nbootstrap=\n---\nreturn {}"
    ).has_value());

    CHECK_FALSE(registry_database_deserialize_record(
        "dnf",
        "source=https://example.test/plugin.lua\nalias=0\ndescription=x\nbundleSource=0\nbundlePath=\nbootstrap=\n---\n<!DOCTYPE html><html></html>"
    ).has_value());
}

TEST_CASE("registry database alias records may omit script payload", "[unit][registry_database][serialization]") {
    const std::string payload =
        "source=apt\n"
        "alias=1\n"
        "description=Alias\n"
        "bundleSource=0\n"
        "bundlePath=\n"
        "bootstrap=\n"
        "---\n";

    const std::optional<RegistryRecord> parsed = registry_database_deserialize_record("yum", payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed->alias);
    CHECK(parsed->source == "apt");
    CHECK(parsed->script.empty());
}
