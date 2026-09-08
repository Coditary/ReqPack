#include <catch2/catch.hpp>

#include "output/run_result_json.h"

TEST_CASE("run result json groups packages by system with status and errors", "[unit][output][json]") {
    RunResultJsonDocument document;
    document.ok = false;
    document.dryRun = false;
    document.items = {
        RunResultJsonItem{.system = "npm", .name = "eslint", .version = "9.0.0", .status = "installed"},
        RunResultJsonItem{.system = "npm", .name = "vitest", .version = "3.3.3", .status = "failed", .errorMessage = "plugin action failed"},
        RunResultJsonItem{.system = "apt", .name = "curl", .version = "8.0.0", .status = "installed"},
    };

    const std::string json = serialize_run_result_json(document);
    CHECK(json.find("\"ok\":false") != std::string::npos);
    CHECK(json.find("\"dryRun\":false") != std::string::npos);
    CHECK(json.find("\"npm\":[") != std::string::npos);
    CHECK(json.find("\"name\":\"eslint\"") != std::string::npos);
    CHECK(json.find("\"version\":\"9.0.0\"") != std::string::npos);
    CHECK(json.find("\"status\":\"installed\"") != std::string::npos);
    CHECK(json.find("\"name\":\"vitest\"") != std::string::npos);
    CHECK(json.find("\"status\":\"failed\"") != std::string::npos);
    CHECK(json.find("\"errorMessage\":\"plugin action failed\"") != std::string::npos);
    CHECK(json.find("\"apt\":[") != std::string::npos);
    CHECK(json.find("\"name\":\"curl\"") != std::string::npos);
}

TEST_CASE("run result status mapping covers install remove update and dry-run", "[unit][output][json]") {
    CHECK(run_result_status_for_action(ActionType::INSTALL, true, false) == "installed");
    CHECK(run_result_status_for_action(ActionType::REMOVE, true, false) == "removed");
    CHECK(run_result_status_for_action(ActionType::UPDATE, true, false) == "updated");
    CHECK(run_result_status_for_action(ActionType::INSTALL, true, true) == "planned");
    CHECK(run_result_status_for_action(ActionType::INSTALL, false, false) == "failed");
    CHECK(run_result_status_skipped(ActionType::INSTALL) == "skipped");
}
