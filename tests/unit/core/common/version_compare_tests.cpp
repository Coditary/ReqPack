#include <catch2/catch.hpp>

#include "core/common/version_compare.h"

TEST_CASE("semver comparator handles prerelease and build metadata", "[unit][version_compare][semver]") {
    const VersionComparatorSpec spec{.profile = "semver"};

    CHECK(version_compare_values("v1.2.3+build.5", "1.2.3", spec) == 0);
    CHECK(version_compare_values("1.0.0-alpha.1", "1.0.0-alpha.beta", spec) < 0);
    CHECK(version_compare_values("1.0.0", "1.0.0-rc.1", spec) > 0);
    CHECK(version_compare_values("1.2", "1.2.0", spec) == 0);
}

TEST_CASE("rpm evr comparator respects epoch tilde and release", "[unit][version_compare][rpm]") {
    const VersionComparatorSpec spec{.profile = "rpm-evr"};

    CHECK(version_compare_values("1:1.2-1", "0:9.9-9", spec) > 0);
    CHECK(version_compare_values("1.0~beta1", "1.0", spec) < 0);
    CHECK(version_compare_values("2.4-3", "2.4-2", spec) > 0);
}

TEST_CASE("pep440 comparator orders dev prerelease final post and local", "[unit][version_compare][pep440]") {
    const VersionComparatorSpec spec{.profile = "pep440"};

    CHECK(version_compare_values("1.0.dev1", "1.0a1", spec) < 0);
    CHECK(version_compare_values("1.0a1", "1.0rc1", spec) < 0);
    CHECK(version_compare_values("1.0rc1", "1.0", spec) < 0);
    CHECK(version_compare_values("1.0", "1.0.post1", spec) < 0);
    CHECK(version_compare_values("1!1.0", "9.9", spec) > 0);
    CHECK(version_compare_values("1.0+local.1", "1.0", spec) > 0);
}

TEST_CASE("maven comparable comparator normalizes common qualifiers", "[unit][version_compare][maven]") {
    const VersionComparatorSpec spec{.profile = "maven-comparable"};

    CHECK(version_compare_values("1", "1.0", spec) == 0);
    CHECK(version_compare_values("1-alpha1", "1-beta1", spec) < 0);
    CHECK(version_compare_values("1-beta1", "1-rc1", spec) < 0);
    CHECK(version_compare_values("1-rc1", "1", spec) < 0);
    CHECK(version_compare_values("1", "1-sp1", spec) < 0);
    CHECK(version_compare_values("1-ga", "1", spec) == 0);
}

TEST_CASE("regex fallback tokenizes custom patterns and honors case mode", "[unit][version_compare][fallback]") {
    const VersionComparatorSpec caseInsensitiveSpec{
        .profile = "",
        .tokenPattern = "[0-9]+|[A-Za-z]+",
        .caseInsensitive = true,
    };
    const VersionComparatorSpec caseSensitiveSpec{
        .profile = "",
        .tokenPattern = "[0-9]+|[A-Za-z]+",
        .caseInsensitive = false,
    };

    CHECK(version_compare_values("pkg-01A", "pkg-1b", caseInsensitiveSpec) < 0);
    CHECK(version_compare_values("RC1", "rc1", caseInsensitiveSpec) == 0);
    CHECK(version_compare_values("RC1", "rc1", caseSensitiveSpec) < 0);
}
