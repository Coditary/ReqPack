#pragma once

#include "core/config/configuration.h"
#include "core/common/types.h"

#include <string>
#include <vector>

struct ValidationFinding {
    std::string id;
    std::string kind{"vulnerability"};
    Package package;
    std::string source;
    std::string severity;
    double score{0.0};
    std::string message;
};

struct ValidationPolicy {
    bool promptOnUnsafe{false};
    std::string abortThreshold{"critical"};
    double abortScoreThreshold{0.0};
    bool generateReport{false};
    UnsafeAction unresolvedVersionAction{UnsafeAction::CONTINUE};
    bool strictEcosystemMapping{false};
};

enum class ValidationDisposition {
    Continue,
    Prompt,
    Abort,
};

ValidationPolicy validator_policy_from_config(const ReqPackConfig& config);
int validator_severity_rank(const std::string& severity);
bool validator_is_blocking_finding(const ValidationFinding& finding, const ValidationPolicy& policy);
std::vector<ValidationFinding> validator_apply_rules(
    const std::vector<ValidationFinding>& findings,
    const std::vector<std::string>& allowIds,
    const std::vector<std::string>& ignoreIds
);
bool validator_findings_exceed_threshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
bool validator_should_prompt_user(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
ValidationDisposition validator_disposition(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
