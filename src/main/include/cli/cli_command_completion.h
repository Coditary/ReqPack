#pragma once

#include "completion/dotted_path.hpp"

#include <string>
#include <vector>

namespace cli_internal {

[[nodiscard]] completion::CompletionTree build_subcommand_completion_tree();

[[nodiscard]] std::vector<std::string> list_subcommand_completions(const std::string& dottedPrefix);

[[nodiscard]] std::vector<std::string> list_global_flag_completions(const std::string& prefix);

}  // namespace cli_internal
