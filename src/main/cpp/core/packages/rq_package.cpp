#include "core/packages/rq_package.h"

#include "rq_package_internal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace {

std::unordered_set<std::string> expand_system_token(
    const std::string& token,
    const std::map<std::string, std::vector<std::string>>& aliases,
    std::unordered_set<std::string>& visiting
) {
    std::unordered_set<std::string> expanded;
    if (token.empty()) {
        return expanded;
    }

    expanded.insert(token);
    if (!visiting.insert(token).second) {
        return expanded;
    }

    if (const auto it = aliases.find(token); it != aliases.end()) {
        for (const std::string& member : it->second) {
            const auto nested = expand_system_token(member, aliases, visiting);
            expanded.insert(nested.begin(), nested.end());
        }
    }

    for (const auto& [alias, members] : aliases) {
        if (std::find(members.begin(), members.end(), token) != members.end()) {
            expanded.insert(alias);
            const auto nested = expand_system_token(alias, aliases, visiting);
            expanded.insert(nested.begin(), nested.end());
        }
    }

    visiting.erase(token);
    return expanded;
}

std::unordered_set<std::string> expand_system_token(
    const std::string& token,
    const std::map<std::string, std::vector<std::string>>& aliases
) {
    std::unordered_set<std::string> visiting;
    return expand_system_token(token, aliases, visiting);
}

void validate_hook_path(const std::string& hookPathText) {
    const std::filesystem::path hookPath(hookPathText);
    if (rq_package_internal::path_has_invalid_segments(hookPath)) {
        throw std::runtime_error("invalid hook path: " + hookPathText);
    }
    rq_package_internal::validate_outer_entry_path(hookPathText);
}

}  // namespace

namespace rq_package_internal {

std::string trim_copy(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> normalize_string_values(const std::vector<std::string>& values) {
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    for (std::string value : values) {
        value = to_lower_copy(trim_copy(std::move(value)));
        if (!value.empty()) {
            normalized.push_back(std::move(value));
        }
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return normalized;
}

bool path_has_invalid_segments(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return true;
    }

    for (const std::filesystem::path& part : path) {
        const std::string token = part.string();
        if (token.empty() || token == "." || token == "..") {
            return true;
        }
    }
    return false;
}

bool exists_no_error(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool is_regular_file_no_error(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool is_directory_no_error(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::absolute(path, error);
    if (error) {
        throw std::runtime_error("failed to resolve path: " + path.string());
    }
    return resolved;
}

std::string path_string(const std::filesystem::path& path) {
    return path.generic_string();
}

void validate_outer_entry_path(const std::string& rawPath) {
    const std::filesystem::path path(rawPath);
    if (path_has_invalid_segments(path)) {
        throw std::runtime_error("invalid archive path: " + rawPath);
    }

    const std::string topLevel = (*path.begin()).string();
    static const std::set<std::string> allowedTopLevels{
        "metadata.json",
        "reqpack.lua",
        "hashes",
        "scripts",
        "payload",
    };

    if (!allowedTopLevels.contains(topLevel)) {
        throw std::runtime_error("unexpected top-level archive entry: " + rawPath);
    }
}

void validate_hook_files(const std::map<std::string, std::string>& hooks, const std::filesystem::path& root) {
    for (const auto& [hookName, hookPathText] : hooks) {
        if (hookPathText.empty()) {
            throw std::runtime_error("hook path missing: " + hookName);
        }
        validate_hook_path(hookPathText);
        if (!is_regular_file_no_error(root / hookPathText)) {
            throw std::runtime_error("hook file not found: " + hookPathText);
        }
    }
}

}  // namespace rq_package_internal

RqMetadata rq_parse_metadata_json(const std::string& content) {
    return rq_package_internal::parse_metadata_json_impl(content);
}

std::map<std::string, std::string> rq_parse_reqpack_hooks(const std::filesystem::path& reqpackLuaPath) {
    return rq_package_internal::parse_reqpack_hooks_impl(reqpackLuaPath);
}

std::string rq_host_architecture() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "armv7";
#else
    return "unknown";
#endif
}

std::string rq_package_identity(const RqMetadata& metadata) {
    return metadata.name + "@" + metadata.version + "-" + std::to_string(metadata.release) + "+r" + std::to_string(metadata.revision);
}

bool rq_architecture_matches(const std::string& packageArchitecture, const std::string& hostArchitecture) {
    return packageArchitecture == "noarch" || packageArchitecture == hostArchitecture;
}

std::string rq_normalize_architecture(std::string architecture) {
    architecture = rq_package_internal::to_lower_copy(rq_package_internal::trim_copy(std::move(architecture)));
    return architecture.empty() ? "noarch" : architecture;
}

std::vector<std::string> rq_normalize_systems(const std::vector<std::string>& systems) {
    std::vector<std::string> normalized = rq_package_internal::normalize_string_values(systems);
    if (normalized.empty()) {
        normalized.push_back("nosys");
    }
    return normalized;
}

std::map<std::string, std::vector<std::string>> rq_builtin_system_aliases() {
    return {
        {"darwin-family", {"darwin", "macos"}},
        {"debian-family", {"debian", "ubuntu", "linuxmint", "pop"}},
        {"rhel-family", {"almalinux", "centos", "fedora", "rhel", "rocky"}},
    };
}

std::map<std::string, std::vector<std::string>> rq_merged_system_aliases(const ReqPackConfig& config) {
    auto aliases = rq_builtin_system_aliases();
    for (const auto& [name, members] : config.rqp.systemAliases) {
        std::vector<std::string> merged = aliases[name];
        merged.insert(merged.end(), members.begin(), members.end());
        aliases[name] = rq_package_internal::normalize_string_values(merged);
    }
    return aliases;
}

std::set<std::string> rq_host_system_tokens(const HostInfoSnapshot& snapshot) {
    std::set<std::string> tokens;
    const std::string family = rq_package_internal::to_lower_copy(rq_package_internal::trim_copy(snapshot.os.family));
    const std::string id = rq_package_internal::to_lower_copy(rq_package_internal::trim_copy(snapshot.os.id));
    const std::string distroId = snapshot.os.distroId.has_value()
        ? rq_package_internal::to_lower_copy(rq_package_internal::trim_copy(snapshot.os.distroId.value()))
        : std::string{};

    if (!family.empty()) {
        tokens.insert(family);
    }
    if (!id.empty()) {
        tokens.insert(id);
    }
    if (!distroId.empty()) {
        tokens.insert(distroId);
    }
    if (tokens.contains("macos")) {
        tokens.insert("darwin");
    }
    if (tokens.contains("darwin")) {
        tokens.insert("macos");
    }
    return tokens;
}

bool rq_system_matches(
    const std::vector<std::string>& packageSystems,
    const std::set<std::string>& hostSystems,
    const std::map<std::string, std::vector<std::string>>& aliases
) {
    for (const std::string& packageSystem : rq_normalize_systems(packageSystems)) {
        if (packageSystem == "nosys") {
            return true;
        }
        const auto expandedPackage = expand_system_token(packageSystem, aliases);
        for (const std::string& hostSystem : hostSystems) {
            if (expandedPackage.contains(hostSystem)) {
                return true;
            }
            const auto expandedHost = expand_system_token(hostSystem, aliases);
            for (const std::string& token : expandedPackage) {
                if (expandedHost.contains(token)) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::string rq_join_systems(const std::vector<std::string>& systems) {
    const std::vector<std::string> normalized = rq_normalize_systems(systems);
    std::ostringstream stream;
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }
        stream << normalized[index];
    }
    return stream.str();
}

std::string rq_metadata_json(const RqMetadata& metadata) {
    return rq_package_internal::metadata_json_impl(metadata);
}

RqPackageLayout RqPackageReader::load(
    const std::filesystem::path& packagePath,
    const std::filesystem::path& workRoot,
    const std::filesystem::path& stateRoot,
    const ReqPackConfig& config,
    const bool validateHostCompatibility
) {
    return rq_package_internal::load_package_layout_impl(packagePath, workRoot, stateRoot, config, validateHostCompatibility);
}

RqPackageBuildResult rq_build_package(const RqPackageBuildRequest& request, const ReqPackConfig& config) {
    return rq_package_internal::build_package_impl(request, config);
}
