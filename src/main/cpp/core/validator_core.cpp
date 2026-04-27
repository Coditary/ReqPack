#include "core/validator_core.h"

ValidationPolicy validator_policy_from_config(const ReqPackConfig& config) {
    ValidationPolicy policy;
    policy.promptOnUnsafe = config.security.promptOnUnsafe ||
        config.security.onUnsafe == UnsafeAction::PROMPT;
    policy.abortThreshold = to_string(config.security.severityThreshold);
    policy.abortScoreThreshold = config.security.scoreThreshold;
    policy.generateReport = config.reports.enabled;
    return policy;
}

int validator_severity_rank(const std::string& severity) {
    if (severity == "critical") {
        return 4;
    }
    if (severity == "high") {
        return 3;
    }
    if (severity == "medium") {
        return 2;
    }
    if (severity == "low") {
        return 1;
    }
    if (severity == "unassigned") {
        return 0;
    }

    return 0;
}

bool validator_findings_exceed_threshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) {
    const int thresholdRank = validator_severity_rank(policy.abortThreshold);
    for (const ValidationFinding& finding : findings) {
        if (validator_severity_rank(finding.severity) >= thresholdRank) {
            return true;
        }

        if (policy.abortScoreThreshold > 0.0 && finding.score >= policy.abortScoreThreshold) {
            return true;
        }
    }

    return false;
}

bool validator_should_prompt_user(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) {
    return policy.promptOnUnsafe && !findings.empty();
}

ValidationDisposition validator_disposition(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) {
    if (validator_findings_exceed_threshold(findings, policy)) {
        return policy.promptOnUnsafe ? ValidationDisposition::Prompt : ValidationDisposition::Abort;
    }

    return validator_should_prompt_user(findings, policy)
        ? ValidationDisposition::Prompt
        : ValidationDisposition::Continue;
}
