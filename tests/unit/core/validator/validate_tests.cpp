#include <catch2/catch.hpp>

#include <boost/graph/adjacency_list.hpp>

#include <vector>

#include "core/validator.h"

namespace {

Graph make_graph() {
    Graph graph;
    boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "dnf", .name = "ripgrep"}, graph);
    return graph;
}

ValidationFinding make_finding(const std::string& severity, double score = 0.0) {
    ValidationFinding finding;
    finding.severity = severity;
    finding.score = score;
    return finding;
}

class TestValidator : public Validator {
public:
    using Validator::Validator;

    std::vector<ValidationFinding> findings;
    bool promptResponse{true};
    mutable int promptCalls{0};
    mutable int reportCalls{0};

protected:
    void expect_same_findings(const std::vector<ValidationFinding>& actual) const {
        REQUIRE(actual.size() == findings.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].severity == findings[i].severity);
            CHECK(actual[i].score == findings[i].score);
            CHECK(actual[i].message == findings[i].message);
            CHECK(actual[i].source == findings[i].source);
        }
    }

    std::vector<ValidationFinding> scanGraph(const Graph& graph) const override {
        (void)graph;
        return findings;
    }

    bool requestUserDecision(const std::vector<ValidationFinding>& requestedFindings) const override {
        expect_same_findings(requestedFindings);
        ++promptCalls;
        return promptResponse;
    }

    void generateReport(const Graph& graph, const std::vector<ValidationFinding>& reportedFindings) const override {
        (void)graph;
        expect_same_findings(reportedFindings);
        ++reportCalls;
    }
};

}  // namespace

TEST_CASE("validator returns nullptr for null graph", "[unit][validator][validate]") {
    Validator validator;
    CHECK(validator.validate(nullptr) == nullptr);
}

TEST_CASE("validator aborts immediately when findings exceed threshold and prompting is disabled", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;

    TestValidator validator(config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == nullptr);
    CHECK(validator.promptCalls == 0);
}

TEST_CASE("validator prompts once when unsafe findings require user approval", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.promptOnUnsafe = true;

    TestValidator validator(config);
    validator.findings = {make_finding("critical")};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == &graph);
    CHECK(validator.promptCalls == 1);
}

TEST_CASE("validator aborts when user declines prompted run", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.security.promptOnUnsafe = true;
    config.security.severityThreshold = SeverityLevel::CRITICAL;

    TestValidator validator(config);
    validator.findings = {make_finding("low")};
    validator.promptResponse = false;

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == nullptr);
    CHECK(validator.promptCalls == 1);
}

TEST_CASE("validator returns input graph and generates report for safe findings", "[unit][validator][validate]") {
    ReqPackConfig config;
    config.reports.enabled = true;

    TestValidator validator(config);
    validator.findings = {};

    Graph graph = make_graph();
    CHECK(validator.validate(&graph) == &graph);
    CHECK(validator.promptCalls == 0);
    CHECK(validator.reportCalls == 1);
}
