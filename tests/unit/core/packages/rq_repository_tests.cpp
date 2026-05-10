#include <catch2/catch.hpp>

#include "core/packages/rq_repository.h"

#include <set>

TEST_CASE("rqp repository resolver prefers highest release and revision", "[unit][rq_repository][core]") {
    const RqRepositoryIndex index = rq_repository_parse_index(
        R"({
  "schemaVersion": 1,
  "packages": [
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 0,
      "architecture": "x86_64",
      "summary": "older",
      "url": "file:///tmp/tool-1.rqp"
    },
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 2,
      "architecture": "x86_64",
      "summary": "newer revision",
      "url": "file:///tmp/tool-2.rqp"
    },
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 2,
      "revision": 0,
      "architecture": "x86_64",
      "summary": "newer release",
      "url": "file:///tmp/tool-3.rqp"
    }
  ]
})",
        "file:///tmp/index.json"
    );

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64", std::set<std::string>{"linux"});

    REQUIRE(resolved.has_value());
    CHECK(resolved->release == 2);
    CHECK(resolved->revision == 0);
    CHECK(resolved->url == "file:///tmp/tool-3.rqp");
}

TEST_CASE("rqp repository resolver skips wrong architecture and accepts noarch", "[unit][rq_repository][core]") {
    const RqRepositoryIndex index = rq_repository_parse_index(
        R"({
  "schemaVersion": 1,
  "packages": [
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 0,
      "architecture": "aarch64",
      "summary": "wrong arch",
      "url": "file:///tmp/tool-aarch64.rqp"
    },
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 1,
      "architecture": "noarch",
      "summary": "portable",
      "url": "file:///tmp/tool-noarch.rqp"
    }
  ]
})",
        "file:///tmp/index.json"
    );

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64", std::set<std::string>{"linux"});

    REQUIRE(resolved.has_value());
    CHECK(resolved->architecture == "noarch");
    CHECK(resolved->url == "file:///tmp/tool-noarch.rqp");
}

TEST_CASE("rqp repository resolver prefers highest version before release and revision", "[unit][rq_repository][core]") {
    const RqRepositoryIndex index = rq_repository_parse_index(
        R"({
  "schemaVersion": 1,
  "packages": [
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 9,
      "revision": 9,
      "architecture": "x86_64",
      "summary": "older version",
      "url": "file:///tmp/tool-1.0.0.rqp"
    },
    {
      "name": "tool",
      "version": "1.1.0",
      "release": 1,
      "revision": 0,
      "architecture": "x86_64",
      "summary": "newer version",
      "url": "file:///tmp/tool-1.1.0.rqp"
    }
  ]
})",
        "file:///tmp/index.json"
    );

    const auto resolved = rq_repository_resolve_package({index}, "tool", {}, "x86_64", std::set<std::string>{"linux"});

    REQUIRE(resolved.has_value());
    CHECK(resolved->version == "1.1.0");
    CHECK(resolved->url == "file:///tmp/tool-1.1.0.rqp");
}

TEST_CASE("rqp repository resolver filters by system tokens", "[unit][rq_repository][core]") {
    const RqRepositoryIndex index = rq_repository_parse_index(
        R"({
  "schemaVersion": 1,
  "packages": [
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 0,
      "architecture": "noarch",
      "system": "fedora",
      "summary": "fedora build",
      "url": "file:///tmp/tool-fedora.rqp"
    },
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 1,
      "architecture": "noarch",
      "system": ["ubuntu", "linux"],
      "summary": "linux build",
      "url": "file:///tmp/tool-linux.rqp"
    }
  ]
})",
        "file:///tmp/index.json"
    );

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64", std::set<std::string>{"debian", "linux"});

    REQUIRE(resolved.has_value());
    CHECK(resolved->url == "file:///tmp/tool-linux.rqp");
}

TEST_CASE("rqp repository resolver supports alias groups", "[unit][rq_repository][core]") {
    ReqPackConfig config = default_reqpack_config();
    config.rqp.systemAliases["lab-family"] = {"nobara", "fedora"};

    const RqRepositoryIndex index = rq_repository_parse_index(
        R"({
  "schemaVersion": 1,
  "packages": [
    {
      "name": "tool",
      "version": "1.0.0",
      "release": 1,
      "revision": 0,
      "architecture": "noarch",
      "system": "lab-family",
      "summary": "lab build",
      "url": "file:///tmp/tool-lab.rqp"
    }
  ]
})",
        "file:///tmp/index.json"
    );

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64", std::set<std::string>{"nobara", "linux"}, config);

    REQUIRE(resolved.has_value());
    CHECK(resolved->url == "file:///tmp/tool-lab.rqp");
}
