#include "cli/cli_command_completion.h"

#include "completion/prefix.hpp"

#include <utility>

namespace cli_internal {

namespace {

void add_leaf(completion::CompletionTree& parent, const char* key) {
    parent.ensure_object_child(key) = completion::CompletionTree::leaf();
}

void add_plugin_subcommands(completion::CompletionTree& plugin) {
    add_leaf(plugin, "update");
    add_leaf(plugin, "ensure");
    add_leaf(plugin, "test");
    add_leaf(plugin, "info");
    add_leaf(plugin, "update-tool");
}

}  // namespace

completion::CompletionTree build_subcommand_completion_tree() {
    completion::CompletionTree root;

    add_leaf(root, "install");
    add_leaf(root, "i");
    add_leaf(root, "remove");
    add_leaf(root, "rm");
    add_leaf(root, "update");
    add_leaf(root, "up");
    add_leaf(root, "search");
    add_leaf(root, "s");
    add_leaf(root, "find");
    add_leaf(root, "list");
    add_leaf(root, "ls");
    add_leaf(root, "outdated");
    add_leaf(root, "out");
    add_leaf(root, "info");
    add_leaf(root, "snapshot");
    add_leaf(root, "pack");
    add_leaf(root, "audit");
    add_leaf(root, "sbom");
    add_leaf(root, "self-update");
    add_leaf(root, "ensure");
    add_leaf(root, "test-plugin");
    auto& live = root.ensure_object_child("live");
    add_leaf(live, "run");
    add_leaf(root, "version");
    add_leaf(root, "serve");
    add_leaf(root, "remote");

    auto& plugin = root.ensure_object_child("plugin");
    add_plugin_subcommands(plugin);

    auto& security = root.ensure_object_child("security");
    add_leaf(security, "status");
    add_leaf(security, "doctor");

    auto& host = root.ensure_object_child("host");
    add_leaf(host, "refresh");

    return root;
}

std::vector<std::string> list_subcommand_completions(const std::string& dottedPrefix) {
    static const completion::CompletionTree tree = build_subcommand_completion_tree();
    return completion::list_dotted_path_completions(tree, dottedPrefix, '.');
}

std::vector<std::string> list_global_flag_completions(const std::string& prefix) {
    static const std::vector<std::string> kGlobalFlags {
        "-h",
        "--help",
        "--help-all",
        "-V",
        "--version",
        "-v",
        "--verbose",
        "-y",
        "--yes",
        "--non-interactive",
        "-n",
        "--dry-run",
        "-c",
        "--config",
        "--json",
        "-j",
        "--jobs",
        "--jobs-max",
        "--registry",
        "--plugin-dir",
        "--no-auto-load-plugins",
        "--no-proxy-expansion",
        "--audit",
        "--no-security",
        "--on-unsafe",
        "--on-unresolved",
        "--prompt-on-unsafe",
        "--abort-on-unsafe",
        "--osv-db",
        "--osv-feed",
        "--osv-refresh",
        "--ignore-vuln",
        "--sbom-format",
        "--sbom-skip-missing-packages",
        "--report-format",
        "--archive-password",
        "--stop-on-first-failure",
        "--no-transaction-db",
        "--install-completion",
        "--dump-completion",
        "--complete-subcommands",
        "--complete-global-flags",
    };

    return completion::filter_by_prefix(kGlobalFlags, prefix);
}

}  // namespace cli_internal
