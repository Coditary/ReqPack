#include "core/security/validator_core.h"

#include <algorithm>

ValidationPolicy validator_policy_from_config(const ReqPackConfig& config) {
    ValidationPolicy policy;
    policy.promptOnUnsafe = config.security.promptOnUnsafe ||
        config.security.onUnsafe == UnsafeAction::PROMPT;
    policy.abortThreshold = to_string(config.security.severityThreshold);
    policy.abortScoreThreshold = config.security.scoreThreshold;
    policy.generateReport = config.reports.enabled;
    policy.unresolvedVersionAction = config.security.onUnresolvedVersion;
    policy.strictEcosystemMapping = config.security.strictEcosystemMapping;
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

bool validator_is_blocking_finding(const ValidationFinding& finding, const ValidationPolicy& policy) {
    if (finding.kind == "sync_error") {
        return true;
    }
    if (finding.kind == "unsupported_ecosystem") {
        return policy.strictEcosystemMapping;
    }
    if (finding.kind == "unresolved_version") {
        return policy.unresolvedVersionAction == UnsafeAction::ABORT;
    }

    return false;
}

std::vector<ValidationFinding> validator_apply_rules(
    const std::vector<ValidationFinding>& findings,
    const std::vector<std::string>& allowIds,
    const std::vector<std::string>& ignoreIds
) {
    std::vector<ValidationFinding> effective;
    effective.reserve(findings.size());

    for (const ValidationFinding& finding : findings) {
        if (!finding.id.empty() && std::find(ignoreIds.begin(), ignoreIds.end(), finding.id) != ignoreIds.end()) {
            continue;
        }
        if (!finding.id.empty() && std::find(allowIds.begin(), allowIds.end(), finding.id) != allowIds.end()) {
            continue;
        }
        effective.push_back(finding);
    }

    return effective;
}

bool validator_findings_exceed_threshold(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) {
    const int thresholdRank = validator_severity_rank(policy.abortThreshold);
    for (const ValidationFinding& finding : findings) {
        if (validator_is_blocking_finding(finding, policy)) {
            return true;
        }
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
    if (findings.empty()) {
        return false;
    }

    if (policy.unresolvedVersionAction == UnsafeAction::PROMPT) {
        const auto unresolvedIt = std::find_if(findings.begin(), findings.end(), [](const ValidationFinding& finding) {
            return finding.kind == "unresolved_version";
        });
        if (unresolvedIt != findings.end()) {
            return true;
        }
    }

    return policy.promptOnUnsafe;
}

ValidationDisposition validator_disposition(const std::vector<ValidationFinding>& findings, const ValidationPolicy& policy) {
    if (validator_findings_exceed_threshold(findings, policy)) {
        return policy.promptOnUnsafe ? ValidationDisposition::Prompt : ValidationDisposition::Abort;
    }

    return validator_should_prompt_user(findings, policy)
        ? ValidationDisposition::Prompt
        : ValidationDisposition::Continue;
}
