#include <catch2/catch.hpp>

#include "core/rq_repository.h"

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

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64");

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

    const auto resolved = rq_repository_resolve_package({index}, "tool", "1.0.0", "x86_64");

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

    const auto resolved = rq_repository_resolve_package({index}, "tool", {}, "x86_64");

    REQUIRE(resolved.has_value());
    CHECK(resolved->version == "1.1.0");
    CHECK(resolved->url == "file:///tmp/tool-1.1.0.rqp");
}
