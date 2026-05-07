#include <catch2/catch.hpp>

#include <map>
#include <vector>

#include "core/planning/planner_core.h"

TEST_CASE("planner expands configured system aliases and preserves unknown systems", "[unit][planner][alias]") {
    const std::vector<Request> requests{
        Request{.action = ActionType::INSTALL, .system = "brew", .packages = {"ripgrep"}},
        Request{.action = ActionType::REMOVE, .system = "custom", .packages = {"pkg"}},
    };
    const std::map<std::string, std::string> aliases{
        {"brew", "apt"},
    };

    const std::vector<Request> expanded = planner_expand_proxies(requests, aliases);
    REQUIRE(expanded.size() == 2);
    CHECK(expanded[0].system == "apt");
    CHECK(expanded[0].packages == std::vector<std::string>{"ripgrep"});
    CHECK(expanded[1].system == "custom");
}

TEST_CASE("planner contains-only-action helper rejects empty and mixed requests", "[unit][planner][action]") {
    CHECK_FALSE(planner_contains_only_action({}, ActionType::ENSURE));

    const std::vector<Request> ensureRequests{
        Request{.action = ActionType::ENSURE, .system = "dnf"},
        Request{.action = ActionType::ENSURE, .system = "maven"},
    };
    CHECK(planner_contains_only_action(ensureRequests, ActionType::ENSURE));

    const std::vector<Request> mixedRequests{
        Request{.action = ActionType::ENSURE, .system = "dnf"},
        Request{.action = ActionType::INSTALL, .system = "dnf", .packages = {"git"}},
    };
    CHECK_FALSE(planner_contains_only_action(mixedRequests, ActionType::ENSURE));
}

TEST_CASE("planner normalizes dependency defaults without overwriting explicit values", "[unit][planner][dependency]") {
    Package implicit;
    implicit.name = "curl";

    const Package normalized = planner_normalize_dependency(implicit, "dnf");
    CHECK(normalized.action == ActionType::INSTALL);
    CHECK(normalized.system == "dnf");
    CHECK(normalized.name == "curl");

    Package explicitValues;
    explicitValues.action = ActionType::REMOVE;
    explicitValues.system = "apt";
    explicitValues.name = "curl";

    const Package preserved = planner_normalize_dependency(explicitValues, "dnf");
    CHECK(preserved.action == ActionType::REMOVE);
    CHECK(preserved.system == "apt");
}

TEST_CASE("planner shapes requested package specifiers into package records", "[unit][planner][request]") {
    const Request request{
        .action = ActionType::INSTALL,
        .system = "brew",
        .flags = {"dry-run"},
    };

    const Package plain = planner_make_requested_package(request, "apt", "ripgrep");
    CHECK(plain.action == ActionType::INSTALL);
    CHECK(plain.system == "apt");
    CHECK(plain.name == "ripgrep");
    CHECK(plain.version.empty());
    CHECK(plain.flags == std::vector<std::string>{"dry-run"});

    const Package versioned = planner_make_requested_package(request, "apt", "ripgrep@14.1");
    CHECK(versioned.name == "ripgrep");
    CHECK(versioned.version == "14.1");

    const Package trailingAt = planner_make_requested_package(request, "apt", "ripgrep@");
    CHECK(trailingAt.name == "ripgrep@");
    CHECK(trailingAt.version.empty());
}

TEST_CASE("planner shapes local install requests without collapsing into normal package names", "[unit][planner][request]") {
    Request request;
    request.action = ActionType::INSTALL;
    request.system = "dnf";
    request.localPath = "/tmp/packages/custom.rpm";
    request.usesLocalTarget = true;
    request.flags = {"force"};

    const Package localPackage = planner_make_local_requested_package(request, "yum");
    CHECK(localPackage.action == ActionType::INSTALL);
    CHECK(localPackage.system == "yum");
    CHECK(localPackage.name == "custom.rpm");
    CHECK(localPackage.sourcePath == "/tmp/packages/custom.rpm");
    CHECK(localPackage.localTarget);
    CHECK(localPackage.flags == std::vector<std::string>{"force"});
}

TEST_CASE("planner missing-package filtering preserves request metadata and rewrites package list", "[unit][planner][filter]") {
    Request request;
    request.action = ActionType::INSTALL;
    request.system = "dnf";
    request.packages = {"git", "ripgrep"};
    request.flags = {"dry-run"};

    const std::vector<Package> missingPackages{
        Package{.action = ActionType::INSTALL, .system = "dnf", .name = "git", .version = "2.0"},
        Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep"},
    };

    const Request filtered = planner_filter_request_to_missing_packages(request, missingPackages);
    CHECK(filtered.action == ActionType::INSTALL);
    CHECK(filtered.system == "dnf");
    CHECK(filtered.flags == std::vector<std::string>{"dry-run"});
    CHECK(filtered.packages == std::vector<std::string>{"git@2.0", "ripgrep"});
}

TEST_CASE("planner install filtering drops request when plugin reports no missing packages", "[unit][planner][filter]") {
    Request request;
    request.action = ActionType::INSTALL;
    request.system = "dnf";
    request.packages = {"git"};

    const std::optional<Request> filtered = planner_filter_install_request(request, {});
    CHECK_FALSE(filtered.has_value());
}

TEST_CASE("planner package specifier helper preserves local source path", "[unit][planner][filter]") {
    Package localPackage;
    localPackage.name = "ignored-name";
    localPackage.sourcePath = "/tmp/packages/custom.rpm";
    localPackage.localTarget = true;

    CHECK(planner_package_specifier_from_package(localPackage) == "/tmp/packages/custom.rpm");

    Package versioned;
    versioned.name = "git";
    versioned.version = "2.1";
    CHECK(planner_package_specifier_from_package(versioned) == "git@2.1");
}
