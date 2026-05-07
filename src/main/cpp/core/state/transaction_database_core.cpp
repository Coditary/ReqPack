#include "core/state/transaction_database_core.h"

#include <cstdint>
#include <sstream>
#include <string_view>

namespace {

std::uint64_t fnv1a_hash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::optional<std::pair<std::string, std::string>> parse_item_key(const std::string& key) {
    constexpr std::string_view prefix = "item:";
    if (key.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    const std::size_t separator = key.find(':', prefix.size());
    if (separator == std::string::npos || separator + 1 >= key.size()) {
        return std::nullopt;
    }

    const std::string runId = key.substr(prefix.size(), separator - prefix.size());
    const std::string itemId = key.substr(separator + 1);
    if (runId.empty() || itemId.empty()) {
        return std::nullopt;
    }

    return std::make_pair(runId, itemId);
}

}  // namespace

std::string transaction_database_package_token(const Package& package) {
    return std::to_string(static_cast<int>(package.action)) + "|" + package.system + "|" + package.name + "|" + package.version + "|" + package.sourcePath + "|" + (package.localTarget ? "1" : "0");
}

std::string transaction_database_item_id_for_package(const Package& package) {
    std::ostringstream stream;
    stream << std::hex << fnv1a_hash(transaction_database_package_token(package));
    return stream.str();
}

std::string transaction_database_run_key(const std::string& runId) {
    return "run:" + runId;
}

std::string transaction_database_item_prefix(const std::string& runId) {
    return "item:" + runId + ":";
}

std::string transaction_database_item_key(const std::string& runId, const Package& package) {
    return transaction_database_item_prefix(runId) + transaction_database_item_id_for_package(package);
}

std::string transaction_database_escape_field(const std::string& value) {
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

std::string transaction_database_unescape_field(const std::string& value) {
    const std::size_t firstEscape = value.find('\\');
    if (firstEscape == std::string::npos) {
        return value;
    }

    std::string unescaped;
    unescaped.reserve(value.size());
    unescaped.append(value, 0, firstEscape);

    bool escaped = false;
    for (std::size_t index = firstEscape; index < value.size(); ++index) {
        const char c = value[index];
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

std::string transaction_database_serialize_run(const TransactionRunRecord& run) {
    std::ostringstream stream;
    stream << "state=" << transaction_database_escape_field(run.state) << '\n';
    stream << "createdAt=" << transaction_database_escape_field(run.createdAt) << '\n';
    stream << "updatedAt=" << transaction_database_escape_field(run.updatedAt) << '\n';
    stream << "flags=";
    for (std::size_t index = 0; index < run.flags.size(); ++index) {
        if (index > 0) {
            stream << ';';
        }
        stream << transaction_database_escape_field(run.flags[index]);
    }
    stream << '\n';
    return stream.str();
}

std::optional<TransactionRunRecord> transaction_database_deserialize_run(const std::string& runId, const std::string& payload) {
    TransactionRunRecord run;
    run.id = runId;

    bool hasState = false;
    bool hasCreatedAt = false;
    bool hasUpdatedAt = false;

    std::istringstream lines(payload);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equals);
        const std::string value = transaction_database_unescape_field(line.substr(equals + 1));
        if (key == "state") {
            run.state = value;
            hasState = true;
        } else if (key == "createdAt") {
            run.createdAt = value;
            hasCreatedAt = true;
        } else if (key == "updatedAt") {
            run.updatedAt = value;
            hasUpdatedAt = true;
        } else if (key == "flags") {
            std::istringstream flags(value);
            std::string flag;
            while (std::getline(flags, flag, ';')) {
                if (!flag.empty()) {
                    run.flags.push_back(flag);
                }
            }
        }
    }

    if (run.id.empty() || !hasState || !hasCreatedAt || !hasUpdatedAt || run.state.empty() || run.createdAt.empty() || run.updatedAt.empty()) {
        return std::nullopt;
    }
    return run;
}

std::string transaction_database_serialize_item(const TransactionItemRecord& item) {
    std::ostringstream stream;
    stream << "sequence=" << item.sequence << '\n';
    stream << "action=" << static_cast<int>(item.package.action) << '\n';
    stream << "system=" << transaction_database_escape_field(item.package.system) << '\n';
    stream << "name=" << transaction_database_escape_field(item.package.name) << '\n';
    stream << "version=" << transaction_database_escape_field(item.package.version) << '\n';
    stream << "sourcePath=" << transaction_database_escape_field(item.package.sourcePath) << '\n';
    stream << "localTarget=" << (item.package.localTarget ? "1" : "0") << '\n';
    stream << "status=" << transaction_database_escape_field(item.status) << '\n';
    stream << "error=" << transaction_database_escape_field(item.errorMessage) << '\n';
    return stream.str();
}

std::optional<TransactionItemRecord> transaction_database_deserialize_item(const std::string& key, const std::string& payload) {
    const std::optional<std::pair<std::string, std::string>> parsedKey = parse_item_key(key);
    if (!parsedKey.has_value()) {
        return std::nullopt;
    }

    TransactionItemRecord item;
    item.runId = parsedKey->first;
    item.itemId = parsedKey->second;

    bool hasSequence = false;
    bool hasAction = false;
    bool hasSystem = false;
    bool hasName = false;
    bool hasVersion = false;
    bool hasSourcePath = false;
    bool hasLocalTarget = false;
    bool hasStatus = false;
    bool hasError = false;

    std::istringstream lines(payload);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string field = line.substr(0, equals);
        const std::string value = transaction_database_unescape_field(line.substr(equals + 1));
        if (field == "sequence") {
            try {
                item.sequence = static_cast<std::size_t>(std::stoull(value));
                hasSequence = true;
            } catch (...) {
                return std::nullopt;
            }
        } else if (field == "action") {
            try {
                item.package.action = static_cast<ActionType>(std::stoi(value));
                hasAction = true;
            } catch (...) {
                return std::nullopt;
            }
        } else if (field == "system") {
            item.package.system = value;
            hasSystem = true;
        } else if (field == "name") {
            item.package.name = value;
            hasName = true;
        } else if (field == "version") {
            item.package.version = value;
            hasVersion = true;
        } else if (field == "sourcePath") {
            item.package.sourcePath = value;
            hasSourcePath = true;
        } else if (field == "localTarget") {
            if (value == "1") {
                item.package.localTarget = true;
            } else if (value == "0") {
                item.package.localTarget = false;
            } else {
                return std::nullopt;
            }
            hasLocalTarget = true;
        } else if (field == "status") {
            item.status = value;
            hasStatus = true;
        } else if (field == "error") {
            item.errorMessage = value;
            hasError = true;
        }
    }

    if (item.runId.empty() || item.itemId.empty() || !hasSequence || !hasAction || !hasSystem || !hasName || !hasVersion || !hasSourcePath || !hasLocalTarget || !hasStatus || !hasError || item.status.empty()) {
        return std::nullopt;
    }
    return item;
}
