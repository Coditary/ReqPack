#include "core/registry_database_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
#include <sstream>
#include <string_view>

namespace {

constexpr std::string_view META_SEPARATOR = "\n---\n";

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool is_github_repository_https_url(const std::string& normalized) {
    if (!starts_with(normalized, "https://github.com/")) {
        return false;
    }

    const std::string path = normalized.substr(std::string{"https://github.com/"}.size());
    if (path.empty()) {
        return false;
    }

    std::size_t slashCount = 0;
    bool hasSegment = false;
    for (char character : path) {
        if (character == '/') {
            ++slashCount;
            continue;
        }
        hasSegment = true;
    }

    return hasSegment && slashCount == 1 && path.front() != '/' && path.back() != '/';
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::uint64_t fnv1a_hash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<std::string> split_lines(const std::string& value) {
    std::vector<std::string> items;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            items.push_back(line);
        }
    }
    return items;
}

std::string join_lines(const std::vector<std::string>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << '\n';
        }
        stream << values[index];
    }
    return stream.str();
}

std::string escape_scope_component(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '|' || c == '\n') {
            escaped.push_back('\\');
            escaped.push_back(c == '\n' ? 'n' : c);
            continue;
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::vector<std::string> split_escaped_fields(const std::string& value) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char c : value) {
        if (!escaped && c == '\\') {
            escaped = true;
            current.push_back(c);
            continue;
        }
        if (!escaped && c == '|') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
        escaped = false;
    }
    fields.push_back(current);
    return fields;
}

bool registry_record_requires_script_hash(const RegistryRecord& record) {
    return registry_database_is_git_source(record.source) || record.source.find("://") != std::string::npos;
}

std::pair<std::string, std::string> registry_record_payload_files(const RegistryRecord& record) {
    if (record.bundleSource && !record.bundlePath.empty() && std::filesystem::exists(record.bundlePath)) {
        const std::filesystem::path bundlePath(record.bundlePath);
        const std::filesystem::path scriptPath = bundlePath / (record.name + ".lua");
        const std::filesystem::path bootstrapPath = bundlePath / "bootstrap.lua";
        return {
            read_text_file(scriptPath),
            std::filesystem::exists(bootstrapPath) ? read_text_file(bootstrapPath) : std::string{}
        };
    }

    return {record.script, record.bootstrapScript};
}

}  // namespace

std::string registry_database_to_lower_copy(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

std::string registry_database_strip_query_fragment(const std::string& value) {
    const std::size_t separator = value.find_first_of("?#");
    return separator == std::string::npos ? value : value.substr(0, separator);
}

bool registry_database_has_non_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

bool registry_database_looks_like_html_document(const std::string& value) {
    const std::string prefix = registry_database_to_lower_copy(value.substr(0, std::min<std::size_t>(value.size(), 512)));
    return prefix.find("<!doctype html") != std::string::npos ||
           prefix.find("<html") != std::string::npos ||
           prefix.find("<head") != std::string::npos ||
           prefix.find("<body") != std::string::npos;
}

bool registry_database_is_valid_plugin_script(const std::string& script) {
    return registry_database_has_non_whitespace(script) && !registry_database_looks_like_html_document(script);
}

bool registry_database_is_valid_sha256(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

std::string registry_database_sha256_hex(const std::string& value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char byte : digest) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

std::string registry_database_serialize_write_scopes(const std::vector<RegistryWriteScope>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << '\n';
        }
        stream << escape_scope_component(values[index].kind) << '|' << escape_scope_component(values[index].value);
    }
    return stream.str();
}

std::vector<RegistryWriteScope> registry_database_deserialize_write_scopes(const std::string& value) {
    std::vector<RegistryWriteScope> scopes;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_escaped_fields(line);
        if (fields.empty()) {
            continue;
        }
        RegistryWriteScope scope;
        scope.kind = registry_database_unescape_field(fields[0]);
        if (fields.size() > 1) {
            scope.value = registry_database_unescape_field(fields[1]);
        }
        scopes.push_back(std::move(scope));
    }
    return scopes;
}

std::string registry_database_serialize_network_scopes(const std::vector<RegistryNetworkScope>& values) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << '\n';
        }
        stream << escape_scope_component(values[index].host) << '|'
               << escape_scope_component(values[index].scheme) << '|'
               << escape_scope_component(values[index].pathPrefix);
    }
    return stream.str();
}

std::vector<RegistryNetworkScope> registry_database_deserialize_network_scopes(const std::string& value) {
    std::vector<RegistryNetworkScope> scopes;
    std::istringstream stream(value);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_escaped_fields(line);
        RegistryNetworkScope scope;
        if (!fields.empty()) {
            scope.host = registry_database_unescape_field(fields[0]);
        }
        if (fields.size() > 1) {
            scope.scheme = registry_database_unescape_field(fields[1]);
        }
        if (fields.size() > 2) {
            scope.pathPrefix = registry_database_unescape_field(fields[2]);
        }
        scopes.push_back(std::move(scope));
    }
    return scopes;
}

bool registry_database_is_git_source(const std::string& source) {
    const std::string normalized = registry_database_to_lower_copy(registry_database_strip_query_fragment(source));
    return starts_with(normalized, "git+") ||
           starts_with(normalized, "git@") ||
           starts_with(normalized, "git://") ||
           starts_with(normalized, "ssh://") ||
           normalized.ends_with(".git") ||
           is_github_repository_https_url(normalized);
}

std::string registry_database_git_source_url(const std::string& source) {
    const std::string raw = starts_with(source, "git+") ? source.substr(4) : source;
    return registry_database_strip_query_fragment(raw);
}

std::string registry_database_git_source_ref(const std::string& source) {
    const std::string raw = starts_with(source, "git+") ? source.substr(4) : source;
    const std::size_t queryStart = raw.find('?');
    const std::size_t fragmentStart = raw.find('#');

    if (queryStart != std::string::npos) {
        const std::size_t queryEnd = fragmentStart == std::string::npos ? raw.size() : fragmentStart;
        const std::string query = raw.substr(queryStart + 1, queryEnd - queryStart - 1);
        std::stringstream stream(query);
        std::string part;
        while (std::getline(stream, part, '&')) {
            if (starts_with(part, "ref=")) {
                return part.substr(4);
            }
        }
    }

    if (fragmentStart != std::string::npos && fragmentStart + 1 < raw.size()) {
        return raw.substr(fragmentStart + 1);
    }

    return {};
}

std::string registry_database_git_source_with_ref(const std::string& source, const std::string& ref) {
    const bool gitPrefixed = starts_with(source, "git+");
    const std::string base = registry_database_git_source_url(source);
    if (ref.empty()) {
        return gitPrefixed ? "git+" + base : base;
    }
    return (gitPrefixed ? "git+" : std::string{}) + base + "?ref=" + ref;
}

std::filesystem::path registry_database_git_repository_cache_path(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
) {
    std::ostringstream stream;
    stream << std::hex << fnv1a_hash(source);
    return default_reqpack_repo_cache_path() / (pluginName + "-" + stream.str());
}

std::vector<std::string> registry_database_extract_git_tags(const std::string& output) {
    std::vector<std::string> tags;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t marker = line.find("refs/tags/");
        if (marker == std::string::npos) {
            continue;
        }
        const std::string tag = line.substr(marker + std::string{"refs/tags/"}.size());
        if (!tag.empty()) {
            tags.push_back(tag);
        }
    }
    return tags;
}

std::string registry_database_escape_field(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '\n') {
            escaped.push_back('\\');
            escaped.push_back(c == '\n' ? 'n' : c);
            continue;
        }
        escaped.push_back(c);
    }
    return escaped;
}

std::string registry_database_unescape_field(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());

    bool escaped = false;
    for (char c : value) {
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (escaped) {
            unescaped.push_back(c == 'n' ? '\n' : c);
            escaped = false;
            continue;
        }
        unescaped.push_back(c);
    }

    return unescaped;
}

bool registry_record_passes_thin_layer_trust(const ReqPackConfig& config, const RegistryRecord& record) {
    if (!config.security.requireThinLayer || record.alias) {
        return true;
    }

    if (record.source.empty() || record.role.empty() || record.privilegeLevel.empty()) {
        return false;
    }

    if (registry_record_requires_script_hash(record) && !registry_database_is_valid_sha256(record.scriptSha256)) {
        return false;
    }

    if (!record.bootstrapSha256.empty() && !registry_database_is_valid_sha256(record.bootstrapSha256)) {
        return false;
    }

    if (registry_database_is_git_source(record.source) && registry_database_git_source_ref(record.source).empty()) {
        return false;
    }

    return true;
}

bool registry_record_matches_expected_hashes(const RegistryRecord& record) {
    if (record.alias) {
        return true;
    }

    if (record.scriptSha256.empty() && record.bootstrapSha256.empty()) {
        return true;
    }

    const auto [script, bootstrapScript] = registry_record_payload_files(record);
    if (!record.scriptSha256.empty()) {
        if (!registry_database_is_valid_sha256(record.scriptSha256) || script.empty()) {
            return false;
        }
        if (registry_database_sha256_hex(script) != registry_database_to_lower_copy(record.scriptSha256)) {
            return false;
        }
    }

    if (!record.bootstrapSha256.empty()) {
        if (!registry_database_is_valid_sha256(record.bootstrapSha256) || bootstrapScript.empty()) {
            return false;
        }
        if (registry_database_sha256_hex(bootstrapScript) != registry_database_to_lower_copy(record.bootstrapSha256)) {
            return false;
        }
    }

    return true;
}

std::string registry_database_serialize_record(const RegistryRecord& record) {
    std::ostringstream stream;
    stream << "source=" << registry_database_escape_field(record.source) << '\n';
    stream << "alias=" << (record.alias ? "1" : "0") << '\n';
    stream << "originPath=" << registry_database_escape_field(record.originPath) << '\n';
    stream << "description=" << registry_database_escape_field(record.description) << '\n';
    stream << "role=" << registry_database_escape_field(record.role) << '\n';
    stream << "capabilities=" << registry_database_escape_field(join_lines(record.capabilities)) << '\n';
    stream << "ecosystemScopes=" << registry_database_escape_field(join_lines(record.ecosystemScopes)) << '\n';
    stream << "writeScopes=" << registry_database_escape_field(registry_database_serialize_write_scopes(record.writeScopes)) << '\n';
    stream << "networkScopes=" << registry_database_escape_field(registry_database_serialize_network_scopes(record.networkScopes)) << '\n';
    stream << "privilegeLevel=" << registry_database_escape_field(record.privilegeLevel) << '\n';
    stream << "scriptSha256=" << registry_database_escape_field(record.scriptSha256) << '\n';
    stream << "bootstrapSha256=" << registry_database_escape_field(record.bootstrapSha256) << '\n';
    stream << "bundleSource=" << (record.bundleSource ? "1" : "0") << '\n';
    stream << "bundlePath=" << registry_database_escape_field(record.bundlePath) << '\n';
    stream << "bootstrap=" << registry_database_escape_field(record.bootstrapScript) << '\n';
    stream << META_SEPARATOR;
    stream << record.script;
    return stream.str();
}

std::optional<RegistryRecord> registry_database_deserialize_record(const std::string& name, const std::string& payload) {
    const std::size_t separator = payload.find(META_SEPARATOR);
    if (separator == std::string::npos) {
        return std::nullopt;
    }

    RegistryRecord record;
    record.name = name;
    record.script = payload.substr(separator + META_SEPARATOR.size());

    std::istringstream headerStream(payload.substr(0, separator));
    std::string line;
    while (std::getline(headerStream, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, equals);
        const std::string value = registry_database_unescape_field(line.substr(equals + 1));
        if (key == "source") {
            record.source = value;
        } else if (key == "alias") {
            if (value == "1") {
                record.alias = true;
            } else if (value == "0") {
                record.alias = false;
            } else {
                return std::nullopt;
            }
        } else if (key == "originPath") {
            record.originPath = value;
        } else if (key == "description") {
            record.description = value;
        } else if (key == "role") {
            record.role = value;
        } else if (key == "capabilities") {
            record.capabilities = split_lines(value);
        } else if (key == "ecosystemScopes") {
            record.ecosystemScopes = split_lines(value);
        } else if (key == "writeScopes") {
            record.writeScopes = registry_database_deserialize_write_scopes(value);
        } else if (key == "networkScopes") {
            record.networkScopes = registry_database_deserialize_network_scopes(value);
        } else if (key == "privilegeLevel") {
            record.privilegeLevel = value;
        } else if (key == "scriptSha256") {
            record.scriptSha256 = value;
        } else if (key == "bootstrapSha256") {
            record.bootstrapSha256 = value;
        } else if (key == "bundleSource") {
            if (value == "1") {
                record.bundleSource = true;
            } else if (value == "0") {
                record.bundleSource = false;
            } else {
                return std::nullopt;
            }
        } else if (key == "bundlePath") {
            record.bundlePath = value;
        } else if (key == "bootstrap") {
            record.bootstrapScript = value;
        }
    }

    if (record.source.empty() && !record.alias) {
        return std::nullopt;
    }
    if (!record.alias && !record.script.empty() && !registry_database_is_valid_plugin_script(record.script)) {
        return std::nullopt;
    }

    return record;
}
