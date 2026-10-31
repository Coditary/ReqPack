#include <catch2/catch.hpp>

#include "output/query_result_json.h"
#include "output/run_result_json.h"

TEST_CASE("query result json groups list items by system", "[unit][output][json]") {
    std::vector<PackageInfo> items;
    items.push_back(PackageInfo {.system = "rqp", .name = "wuji-ai", .version = "1.0.0"});
    items.push_back(PackageInfo {.system = "npm", .name = "react", .version = "18.3.1"});

    const RunResultJsonDocument document = build_query_packages_document(ActionType::LIST, items);
    const std::string json = serialize_run_result_json(document);

    CHECK(json.find("\"ok\":true") != std::string::npos);
    CHECK(json.find("\"rqp\":[") != std::string::npos);
    CHECK(json.find("\"name\":\"wuji-ai\"") != std::string::npos);
    CHECK(json.find("\"status\":\"listed\"") != std::string::npos);
    CHECK(json.find("\"npm\":[") != std::string::npos);
}

TEST_CASE("query result json includes latestVersion for outdated items", "[unit][output][json]") {
    std::vector<PackageInfo> items;
    items.push_back(PackageInfo {.system = "dnf", .name = "curl", .version = "7.0", .latestVersion = "8.0", .installed = "7.0"});

    const std::string json = serialize_run_result_json(build_query_packages_document(ActionType::OUTDATED, items));
    CHECK(json.find("\"latestVersion\":\"8.0\"") != std::string::npos);
    CHECK(json.find("\"status\":\"outdated\"") != std::string::npos);
}
