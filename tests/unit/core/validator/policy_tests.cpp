#include <catch2/catch.hpp>

#include <vector>

#include "core/validator_core.h"

namespace {

ValidationFinding make_finding(const std::string& severity, double score = 0.0) {
    ValidationFinding finding;
    finding.id = severity + std::to_string(score);
    finding.severity = severity;
    finding.score = score;
    return finding;
}

}  // namespace

TEST_CASE("validator derives policy from config", "[unit][validator][policy]") {
    ReqPackConfig config;
    config.security.onUnsafe = UnsafeAction::PROMPT;
    config.security.severityThreshold = SeverityLevel::HIGH;
    config.security.scoreThreshold = 7.5;
    config.reports.enabled = true;
    config.security.onUnresolvedVersion = UnsafeAction::PROMPT;
    config.security.strictEcosystemMapping = true;

    const ValidationPolicy policy = validator_policy_from_config(config);
    CHECK(policy.promptOnUnsafe);
    CHECK(policy.abortThreshold == "high");
    CHECK(policy.abortScoreThreshold == 7.5);
    CHECK(policy.generateReport);
    CHECK(policy.unresolvedVersionAction == UnsafeAction::PROMPT);
    CHECK(policy.strictEcosystemMapping);
}

TEST_CASE("validator severity ranking matches configured order", "[unit][validator][policy]") {
    CHECK(validator_severity_rank("critical") == 4);
    CHECK(validator_severity_rank("high") == 3);
    CHECK(validator_severity_rank("medium") == 2);
    CHECK(validator_severity_rank("low") == 1);
    CHECK(validator_severity_rank("unassigned") == 0);
    CHECK(validator_severity_rank("unknown") == 0);
}

TEST_CASE("validator threshold checks severity and score", "[unit][validator][policy]") {
    const std::vector<ValidationFinding> findings{
        make_finding("low", 3.0),
        make_finding("medium", 8.1),
    };

    ValidationPolicy severityPolicy;
    severityPolicy.abortThreshold = "high";
    CHECK_FALSE(validator_findings_exceed_threshold(findings, severityPolicy));

    ValidationPolicy scorePolicy;
    scorePolicy.abortThreshold = "critical";
    scorePolicy.abortScoreThreshold = 8.0;
    CHECK(validator_findings_exceed_threshold(findings, scorePolicy));

    ValidationPolicy severityHitPolicy;
    severityHitPolicy.abortThreshold = "medium";
    CHECK(validator_findings_exceed_threshold(findings, severityHitPolicy));
}

TEST_CASE("validator disposition distinguishes continue, prompt, and abort", "[unit][validator][policy]") {
    const std::vector<ValidationFinding> findings{make_finding("medium", 0.0)};

    ValidationPolicy continuePolicy;
    continuePolicy.abortThreshold = "critical";
    CHECK(validator_disposition(findings, continuePolicy) == ValidationDisposition::Continue);

    ValidationPolicy promptPolicy;
    promptPolicy.abortThreshold = "critical";
    promptPolicy.promptOnUnsafe = true;
    CHECK(validator_disposition(findings, promptPolicy) == ValidationDisposition::Prompt);

    ValidationPolicy abortPolicy;
    abortPolicy.abortThreshold = "medium";
    CHECK(validator_disposition(findings, abortPolicy) == ValidationDisposition::Abort);

    ValidationPolicy thresholdPromptPolicy;
    thresholdPromptPolicy.abortThreshold = "medium";
    thresholdPromptPolicy.promptOnUnsafe = true;
    CHECK(validator_disposition(findings, thresholdPromptPolicy) == ValidationDisposition::Prompt);
}

TEST_CASE("validator rules and typed findings affect blocking behavior", "[unit][validator][policy]") {
    const ValidationFinding unsupported = ValidationFinding{
        .id = "dnf",
        .kind = "unsupported_ecosystem",
        .severity = "low",
        .score = 0.0,
    };
    const ValidationFinding unresolved = ValidationFinding{
        .id = "pkg",
        .kind = "unresolved_version",
        .severity = "low",
        .score = 0.0,
    };

    ValidationPolicy defaultPolicy;
    CHECK_FALSE(validator_is_blocking_finding(unsupported, defaultPolicy));
    CHECK_FALSE(validator_is_blocking_finding(unresolved, defaultPolicy));

    ValidationPolicy strictPolicy;
    strictPolicy.strictEcosystemMapping = true;
    CHECK(validator_is_blocking_finding(unsupported, strictPolicy));

    ValidationPolicy unresolvedAbortPolicy;
    unresolvedAbortPolicy.unresolvedVersionAction = UnsafeAction::ABORT;
    CHECK(validator_is_blocking_finding(unresolved, unresolvedAbortPolicy));

    const std::vector<ValidationFinding> findings{
        ValidationFinding{.id = "CVE-1", .kind = "vulnerability", .severity = "critical", .score = 9.8},
        ValidationFinding{.id = "CVE-2", .kind = "vulnerability", .severity = "high", .score = 7.5},
    };
    const std::vector<ValidationFinding> filtered = validator_apply_rules(findings, {"CVE-1"}, {});
    REQUIRE(filtered.size() == 1);
    CHECK(filtered[0].id == "CVE-2");
}
