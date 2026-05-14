#pragma once

#include "output/diagnostic.h"

#include <cstddef>
#include <string>

DiagnosticMessage config_override_diagnostic(const std::string& details);
DiagnosticMessage stdin_syntax_diagnostic(std::size_t lineNumber, const std::string& commandText = {});
DiagnosticMessage stdin_parse_diagnostic(std::size_t lineNumber, const std::string& commandText);
DiagnosticMessage stdin_action_only_diagnostic(std::size_t lineNumber, const std::string& action);
DiagnosticMessage stdin_empty_batch_diagnostic(const std::string& action);
DiagnosticMessage self_update_diagnostic(const std::string& summary,
                                         const std::string& cause,
                                         const std::string& recommendation,
                                         const std::string& details = {});
DiagnosticMessage plugin_test_diagnostic(const std::string& summary,
                                         const std::string& cause,
                                         const std::string& recommendation,
                                         const std::string& details = {});
