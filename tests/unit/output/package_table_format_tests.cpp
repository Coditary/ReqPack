#include <catch2/catch.hpp>

#include "core/common/types.h"
#include "output/package_table_format.h"

TEST_CASE("package table formatting strips boilerplate and merges versions", "[unit][output][package-table]") {
    PackageInfo primary;
    primary.name = "beez-cli";
    primary.version = "1.0.5";
    primary.packageType = "package";
    primary.architecture = "x86_64";
    primary.targetSystems = "linux";
    primary.summary = "Beez build and task orchestrator";

    PackageInfo older;
    older.name = "beez-cli";
    older.version = "1.0.4";
    older.packageType = "package";
    older.architecture = "x86_64";
    older.targetSystems = "linux";
    older.summary = "Beez build and task orchestrator";

    PackageInfo artifact;
    artifact.name = "/tmp/reqpack/teez-0.1.0-linux-x86_64.rqp";
    artifact.version = "0.1.0";
    artifact.packageType = "alias";
    artifact.architecture = "x86_64";
    artifact.targetSystems = "linux";
    artifact.summary = "Unified test runner CLI";

    PackageInfo canonical;
    canonical.name = "teez";
    canonical.version = "0.1.1";
    canonical.packageType = "alias";
    canonical.architecture = "x86_64";
    canonical.targetSystems = "linux";
    canonical.summary = "Unified test runner CLI";

    const std::vector<PreparedListRow> rows =
        prepare_list_table_rows({primary, older, artifact, canonical});
    REQUIRE(rows.size() == 2);

    const auto beez = std::find_if(rows.begin(), rows.end(), [](const PreparedListRow& row) {
        return row.name == "beez-cli";
    });
    REQUIRE(beez != rows.end());
    CHECK(beez->version == "1.0.5 (+1)");
    CHECK(beez->platform == "linux/x86_64");
    CHECK(beez->typeBadge == "pkg");

    const auto teez = std::find_if(rows.begin(), rows.end(), [](const PreparedListRow& row) {
        return row.name == "teez";
    });
    REQUIRE(teez != rows.end());
    CHECK(teez->typeBadge == "alias");
}

TEST_CASE("package search rows disambiguate colliding names and alias targets", "[unit][output][package-table]") {
    PackageInfo pkg;
    pkg.name = "wuji";
    pkg.version = "0.1.3";
    pkg.packageType = "package";
    pkg.summary = "Unified AI CLI";
    pkg.installed = "true";

    PackageInfo manager;
    manager.name = "wuji";
    manager.version = "0.1.0";
    manager.packageType = "package-manager";
    manager.summary = "ReqPack package manager";

    PackageInfo aliasPkg;
    aliasPkg.name = "wuji-ai";
    aliasPkg.version = "0.1.3";
    aliasPkg.packageType = "alias";
    aliasPkg.summary = "Alias for wuji";

    const std::vector<PreparedSearchRow> rows = prepare_search_table_rows({pkg, manager, aliasPkg}, false);
    REQUIRE(rows.size() == 3);

    const auto pkgRow = std::find_if(rows.begin(), rows.end(), [](const PreparedSearchRow& row) {
        return row.typeBadge == "pkg";
    });
    REQUIRE(pkgRow != rows.end());
    CHECK(pkgRow->name == "wuji");
    CHECK(pkgRow->installed);
    CHECK(pkgRow->description.find("[installed]") != std::string::npos);

    const auto managerRow = std::find_if(rows.begin(), rows.end(), [](const PreparedSearchRow& row) {
        return row.typeBadge == "pkg-mgr";
    });
    REQUIRE(managerRow != rows.end());
    CHECK(managerRow->name == "pkg-mgr/wuji");

    const auto aliasRow = std::find_if(rows.begin(), rows.end(), [](const PreparedSearchRow& row) {
        return row.typeBadge == "alias";
    });
    REQUIRE(aliasRow != rows.end());
    CHECK(aliasRow->description.find("-> wuji") != std::string::npos);
}

TEST_CASE("package table formatting shortens package manager type", "[unit][output][package-table]") {
    CHECK(package_table_type_badge("package-manager") == "pkg-mgr");
    PackageInfo plugin;
    plugin.summary = "ReqPack Lua wrapper plugin for Fedora DNF";
    CHECK(package_table_clean_description(plugin) == "Fedora DNF");
}
