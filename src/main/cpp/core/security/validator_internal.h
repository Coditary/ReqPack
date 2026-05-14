#pragma once

#include "core/security/validator.h"

namespace validator_internal {

std::vector<ValidationFinding> disposition_findings(const Graph& graph, const std::vector<ValidationFinding>& findings);

}  // namespace validator_internal
