#include "core/registry_database_core.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace {

constexpr std::string_view META_SEPARATOR = "\n---\n";

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::uint64_t fnv1a_hash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
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

bool registry_database_is_git_source(const std::string& source) {
    const std::string normalized = registry_database_to_lower_copy(registry_database_strip_query_fragment(source));
    return starts_with(normalized, "git+") ||
           starts_with(normalized, "git@") ||
           starts_with(normalized, "git://") ||
           starts_with(normalized, "ssh://") ||
           normalized.ends_with(".git");
}

std::string registry_database_git_source_url(const std::string& source) {
    return starts_with(source, "git+") ? source.substr(4) : source;
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

std::string registry_database_serialize_record(const RegistryRecord& record) {
    std::ostringstream stream;
    stream << "source=" << registry_database_escape_field(record.source) << '\n';
    stream << "alias=" << (record.alias ? "1" : "0") << '\n';
    stream << "description=" << registry_database_escape_field(record.description) << '\n';
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
        } else if (key == "description") {
            record.description = value;
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
