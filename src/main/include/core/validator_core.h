#pragma once

#include "core/configuration.h"
#include "core/types.h"

#include <string>
#include <vector>

struct ValidationFinding {
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
};

enum class ValidationDisposition {
    Continue,
    Prompt,
    Abort,
};

ValidationPolicy validator_policy_from_config(const ReqPackConfig& config);
int validator_severity_rank(const std::string& severity);
bool validator_findings_exceed_threshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
bool validator_should_prompt_user(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
ValidationDisposition validator_disposition(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy);
