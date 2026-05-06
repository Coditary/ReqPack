#include "core/history_manager.h"

#include <lmdb.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr unsigned int INSTALLED_STATE_MAX_DATABASES = 1;
constexpr std::size_t INSTALLED_STATE_MAP_SIZE = 8 * 1024 * 1024;
constexpr std::string_view INSTALLED_STATE_KEY_PREFIX = "pkg:";
constexpr std::string_view INSTALLED_STATE_INITIALIZED_KEY = "meta:initialized";

std::string utc_timestamp_now() {
    const auto now  = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Minimal JSON string escaping — no external dependency.
std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string jstr(const std::string& key, const std::string& value) {
    return "\"" + escape_json(key) + "\":\"" + escape_json(value) + "\"";
}

std::string escape_field(const std::string& value) {
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

std::string unescape_field(const std::string& value) {
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

// Extract the value of a JSON string field from a flat (single-line) JSON object.
// Returns empty string when the key is absent.
std::string extract_json_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const std::size_t start = pos + needle.size();
    std::string value;
    bool escaped = false;
    for (std::size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"':  value += '"';  break;
                case '\\': value += '\\'; break;
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                default:   value += c;    break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            value += c;
        }
    }
    return value;
}

// Read all non-empty lines from a file.
std::vector<std::string> read_all_lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file.is_open()) {
        return lines;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

// Overwrite a file with the given lines (each terminated by '\n').
bool write_all_lines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    for (const std::string& line : lines) {
        file << line << '\n';
    }
    return file.good();
}

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::vector<InstalledEntry> read_legacy_installed_state(const std::filesystem::path& path) {
    std::vector<InstalledEntry> entries;
    if (!std::filesystem::exists(path)) {
        return entries;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"name\"") == std::string::npos) {
            continue;
        }
        InstalledEntry entry;
        entry.name        = extract_json_string(line, "name");
        entry.version     = extract_json_string(line, "version");
        entry.system      = extract_json_string(line, "manager");
        entry.installedAt = extract_json_string(line, "installedAt");
        if (!entry.name.empty() && !entry.system.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

std::string serialize_installed_entry(const InstalledEntry& entry) {
    std::ostringstream stream;
    stream << "name=" << escape_field(entry.name) << '\n';
    stream << "version=" << escape_field(entry.version) << '\n';
    stream << "system=" << escape_field(entry.system) << '\n';
    stream << "installedAt=" << escape_field(entry.installedAt) << '\n';
    stream << "installMethod=" << escape_field(entry.installMethod);
    for (const std::string& owner : entry.owners) {
        stream << '\n' << "owner=" << escape_field(owner);
    }
    return stream.str();
}

std::vector<std::string> normalize_owners(std::vector<std::string> owners) {
    owners.erase(std::remove_if(owners.begin(), owners.end(), [](const std::string& owner) {
        return owner.empty();
    }), owners.end());
    std::sort(owners.begin(), owners.end());
    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    return owners;
}

bool owner_has_prefix(const std::string& owner, std::string_view prefix) {
    return starts_with(owner, std::string(prefix) + "\n");
}

std::string install_method_from_owners(const std::vector<std::string>& owners) {
    const bool hasExplicit = std::any_of(owners.begin(), owners.end(), [](const std::string& owner) {
        return owner_has_prefix(owner, "root");
    });
    const bool hasDependency = std::any_of(owners.begin(), owners.end(), [](const std::string& owner) {
        return owner_has_prefix(owner, "pkg");
    });

    if (hasExplicit && hasDependency) {
        return "explicit+dependency";
    }
    if (hasExplicit) {
        return "explicit";
    }
    if (hasDependency) {
        return "dependency";
    }
    return "unknown";
}

std::optional<InstalledEntry> deserialize_installed_entry(const std::string& payload) {
    InstalledEntry entry;
    std::istringstream stream(payload);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, equals);
        const std::string value = unescape_field(line.substr(equals + 1));
        if (key == "name") {
            entry.name = value;
        } else if (key == "version") {
            entry.version = value;
        } else if (key == "system") {
            entry.system = value;
        } else if (key == "installedAt") {
            entry.installedAt = value;
        } else if (key == "installMethod") {
            entry.installMethod = value;
        } else if (key == "owner") {
            entry.owners.push_back(value);
        }
    }

    if (entry.name.empty() || entry.system.empty()) {
        return std::nullopt;
    }
    entry.owners = normalize_owners(std::move(entry.owners));
    if (entry.installMethod.empty()) {
        entry.installMethod = install_method_from_owners(entry.owners);
    }
    return entry;
}

std::string installed_state_key(const std::string& system, const std::string& name, const std::string& version) {
    return std::string(INSTALLED_STATE_KEY_PREFIX) + escape_field(system) + '\n' + escape_field(name) + '\n' + escape_field(version);
}

std::string installed_state_system_prefix(const std::string& system) {
    return std::string(INSTALLED_STATE_KEY_PREFIX) + escape_field(system) + '\n';
}

std::string installed_state_name_prefix(const std::string& system, const std::string& name) {
    return installed_state_system_prefix(system) + escape_field(name) + '\n';
}

std::string installed_state_identity(const InstalledEntry& entry) {
    return escape_field(entry.name) + '\n' + escape_field(entry.version);
}

struct InstalledStateEnvironment {
    MDB_env* env{nullptr};
    MDB_dbi dbi{0};
    bool dbiOpen{false};

    InstalledStateEnvironment() = default;
    InstalledStateEnvironment(const InstalledStateEnvironment&) = delete;
    InstalledStateEnvironment& operator=(const InstalledStateEnvironment&) = delete;

    InstalledStateEnvironment(InstalledStateEnvironment&& other) noexcept
        : env(other.env), dbi(other.dbi), dbiOpen(other.dbiOpen) {
        other.env = nullptr;
        other.dbi = 0;
        other.dbiOpen = false;
    }

    InstalledStateEnvironment& operator=(InstalledStateEnvironment&& other) noexcept {
        if (this != &other) {
            close();
            env = other.env;
            dbi = other.dbi;
            dbiOpen = other.dbiOpen;
            other.env = nullptr;
            other.dbi = 0;
            other.dbiOpen = false;
        }
        return *this;
    }

    ~InstalledStateEnvironment() {
        close();
    }

    explicit operator bool() const {
        return env != nullptr && dbiOpen;
    }

    void close() {
        if (env != nullptr) {
            if (dbiOpen) {
                mdb_dbi_close(env, dbi);
            }
            mdb_env_close(env);
            env = nullptr;
            dbi = 0;
            dbiOpen = false;
        }
    }
};

InstalledStateEnvironment open_installed_state_environment(
    const std::filesystem::path& dbDirectory,
    bool createIfMissing
) {
    InstalledStateEnvironment environment;
    if (!createIfMissing && !std::filesystem::exists(dbDirectory)) {
        return environment;
    }

    if (createIfMissing) {
        std::error_code directoryError;
        std::filesystem::create_directories(dbDirectory, directoryError);
        if (directoryError) {
            return environment;
        }
    }

    if (mdb_env_create(&environment.env) != MDB_SUCCESS) {
        environment.env = nullptr;
        return environment;
    }

    mdb_env_set_maxdbs(environment.env, INSTALLED_STATE_MAX_DATABASES);
    mdb_env_set_mapsize(environment.env, INSTALLED_STATE_MAP_SIZE);
    if (mdb_env_open(environment.env, dbDirectory.string().c_str(), 0, 0664) != MDB_SUCCESS) {
        environment.close();
        return environment;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        environment.close();
        return environment;
    }

    if (mdb_dbi_open(transaction, nullptr, MDB_CREATE, &environment.dbi) != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        environment.close();
        return environment;
    }
    environment.dbiOpen = true;

    if (mdb_txn_commit(transaction) != MDB_SUCCESS) {
        environment.close();
        return environment;
    }

    return environment;
}

std::optional<std::string> read_value(MDB_txn* transaction, MDB_dbi dbi, const std::string& key) {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val value;
    if (mdb_get(transaction, dbi, &keyValue, &value) != MDB_SUCCESS) {
        return std::nullopt;
    }
    return std::string(static_cast<const char*>(value.mv_data), value.mv_size);
}

bool put_value(MDB_txn* transaction, MDB_dbi dbi, const std::string& key, const std::string& value) {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val dataValue{value.size(), const_cast<char*>(value.data())};
    return mdb_put(transaction, dbi, &keyValue, &dataValue, 0) == MDB_SUCCESS;
}

bool delete_value(MDB_txn* transaction, MDB_dbi dbi, const std::string& key) {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    const int result = mdb_del(transaction, dbi, &keyValue, nullptr);
    return result == MDB_SUCCESS || result == MDB_NOTFOUND;
}

std::vector<std::string> collect_keys_with_prefix(MDB_txn* transaction, MDB_dbi dbi, const std::string& prefix) {
    std::vector<std::string> keys;

    MDB_cursor* cursor = nullptr;
    if (mdb_cursor_open(transaction, dbi, &cursor) != MDB_SUCCESS) {
        return keys;
    }

    MDB_val key{prefix.size(), const_cast<char*>(prefix.data())};
    MDB_val value;
    int result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
    while (result == MDB_SUCCESS) {
        const std::string currentKey(static_cast<const char*>(key.mv_data), key.mv_size);
        if (!starts_with(currentKey, prefix)) {
            break;
        }
        keys.push_back(currentKey);
        result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    return keys;
}

bool delete_keys(MDB_txn* transaction, MDB_dbi dbi, const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        if (!delete_value(transaction, dbi, key)) {
            return false;
        }
    }
    return true;
}

std::vector<InstalledEntry> load_entries_for_keys(
    MDB_txn* transaction,
    MDB_dbi dbi,
    const std::vector<std::string>& keys
) {
    std::vector<InstalledEntry> entries;
    entries.reserve(keys.size());
    for (const std::string& key : keys) {
        const std::optional<std::string> payload = read_value(transaction, dbi, key);
        if (!payload.has_value()) {
            continue;
        }
        if (const std::optional<InstalledEntry> entry = deserialize_installed_entry(payload.value())) {
            entries.push_back(entry.value());
        }
    }
    return entries;
}

std::optional<std::pair<std::string, InstalledEntry>> find_installed_state_entry(
    MDB_txn* transaction,
    MDB_dbi dbi,
    const std::string& system,
    const std::string& name,
    const std::string& version
) {
    const std::vector<std::string> keys = collect_keys_with_prefix(transaction, dbi, installed_state_name_prefix(system, name));
    if (keys.empty()) {
        return std::nullopt;
    }

    const std::vector<InstalledEntry> entries = load_entries_for_keys(transaction, dbi, keys);
    if (!version.empty()) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].version == version) {
                return std::make_pair(keys[index], entries[index]);
            }
        }
    }

    if (entries.size() == 1) {
        return std::make_pair(keys.front(), entries.front());
    }

    return std::nullopt;
}

std::optional<InstalledEntry> select_existing_entry_for_replace(
    const std::map<std::string, InstalledEntry>& existingByIdentity,
    const std::map<std::string, std::vector<InstalledEntry>>& existingByName,
    const InstalledEntry& candidate
) {
    const auto identity = existingByIdentity.find(installed_state_identity(candidate));
    if (identity != existingByIdentity.end()) {
        return identity->second;
    }

    const auto sameName = existingByName.find(candidate.name);
    if (sameName != existingByName.end() && sameName->second.size() == 1) {
        return sameName->second.front();
    }

    return std::nullopt;
}

bool ensure_legacy_installed_state_imported(
    const InstalledStateEnvironment& environment,
    const std::filesystem::path& legacyPath
) {
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    if (read_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY)).has_value()) {
        mdb_txn_abort(transaction);
        return true;
    }

    if (!std::filesystem::exists(legacyPath)) {
        mdb_txn_abort(transaction);
        return true;
    }

    const std::vector<InstalledEntry> legacyEntries = read_legacy_installed_state(legacyPath);
    for (const InstalledEntry& entry : legacyEntries) {
        if (!put_value(transaction, environment.dbi, installed_state_key(entry.system, entry.name, entry.version), serialize_installed_entry(entry))) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1")) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

std::vector<InstalledEntry> read_installed_entries(const InstalledStateEnvironment& environment) {
    std::vector<InstalledEntry> entries;

    MDB_txn* transaction = nullptr;
    MDB_cursor* cursor = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
        return entries;
    }

    if (mdb_cursor_open(transaction, environment.dbi, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        return entries;
    }

    MDB_val key;
    MDB_val value;
    int result = mdb_cursor_get(cursor, &key, &value, MDB_FIRST);
    while (result == MDB_SUCCESS) {
        const std::string keyString(static_cast<const char*>(key.mv_data), key.mv_size);
        if (starts_with(keyString, INSTALLED_STATE_KEY_PREFIX)) {
            const std::string payload(static_cast<const char*>(value.mv_data), value.mv_size);
            if (const std::optional<InstalledEntry> entry = deserialize_installed_entry(payload)) {
                entries.push_back(entry.value());
            }
        }

        result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(transaction);
    return entries;
}

std::vector<InstalledEntry> load_installed_state_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath
) {
    const bool shouldOpenStore = std::filesystem::exists(dbDirectory) || std::filesystem::exists(legacyPath);
    if (!shouldOpenStore) {
        return {};
    }

    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return {};
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return {};
    }

    return read_installed_entries(environment);
}

bool upsert_installed_state_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath,
    const InstalledEntry& entry
) {
    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return false;
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
        !put_value(transaction, environment.dbi, installed_state_key(entry.system, entry.name, entry.version), serialize_installed_entry(entry))) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool remove_installed_state_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath,
    const std::string& system,
    const std::string& name,
    const std::string& version
) {
    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return false;
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    const std::vector<std::string> keys = version.empty()
        ? collect_keys_with_prefix(transaction, environment.dbi, installed_state_name_prefix(system, name))
        : std::vector<std::string>{installed_state_key(system, name, version)};

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
        !delete_keys(transaction, environment.dbi, keys)) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool replace_installed_state_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath,
    const std::string& system,
    const std::vector<InstalledEntry>& entries
) {
    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return false;
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    const std::vector<std::string> existingKeys = collect_keys_with_prefix(transaction, environment.dbi, installed_state_system_prefix(system));
    std::map<std::string, InstalledEntry> existingByIdentity;
    std::map<std::string, std::vector<InstalledEntry>> existingByName;
    for (const std::string& key : existingKeys) {
        const std::optional<std::string> payload = read_value(transaction, environment.dbi, key);
        if (!payload.has_value()) {
            continue;
        }
        if (const auto entry = deserialize_installed_entry(payload.value())) {
            existingByIdentity[installed_state_identity(entry.value())] = entry.value();
            existingByName[entry->name].push_back(entry.value());
        }
    }

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
        !delete_keys(transaction, environment.dbi, existingKeys)) {
        mdb_txn_abort(transaction);
        return false;
    }

    const std::string syncedAt = utc_timestamp_now();
    for (InstalledEntry entry : entries) {
        if (entry.name.empty()) {
            continue;
        }

        entry.system = system;
        const std::optional<InstalledEntry> existing = select_existing_entry_for_replace(existingByIdentity, existingByName, entry);
        if (entry.installedAt.empty()) {
            entry.installedAt = existing.has_value() ? existing->installedAt : syncedAt;
        }
        if (entry.owners.empty() && existing.has_value()) {
            entry.owners = existing->owners;
        }
        entry.owners = normalize_owners(std::move(entry.owners));
        if (entry.installMethod.empty() || entry.installMethod == "unknown") {
            entry.installMethod = existing.has_value() && !existing->installMethod.empty()
                ? existing->installMethod
                : install_method_from_owners(entry.owners);
        }

        if (!put_value(transaction, environment.dbi, installed_state_key(entry.system, entry.name, entry.version), serialize_installed_entry(entry))) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool merge_installed_ownership_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath,
    const Package& package,
    const std::vector<std::string>& ownerIds,
    bool directRequest
) {
    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return false;
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    const std::optional<std::pair<std::string, InstalledEntry>> existing = find_installed_state_entry(
        transaction,
        environment.dbi,
        package.system,
        package.name,
        package.version
    );
    if (!existing.has_value()) {
        mdb_txn_abort(transaction);
        return false;
    }

    InstalledEntry merged = existing->second;
    merged.owners.insert(merged.owners.end(), ownerIds.begin(), ownerIds.end());
    merged.owners = normalize_owners(std::move(merged.owners));
    merged.installMethod = install_method_from_owners(merged.owners);
    if (directRequest && merged.installMethod == "unknown") {
        merged.installMethod = "explicit";
    }

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
        !put_value(transaction, environment.dbi, existing->first, serialize_installed_entry(merged))) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool subtract_installed_ownership_database(
    const std::filesystem::path& dbDirectory,
    const std::filesystem::path& legacyPath,
    const Package& package,
    const std::vector<std::string>& ownerIds
) {
    InstalledStateEnvironment environment = open_installed_state_environment(dbDirectory, true);
    if (!environment) {
        return false;
    }
    if (!ensure_legacy_installed_state_imported(environment, legacyPath)) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(environment.env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    const std::optional<std::pair<std::string, InstalledEntry>> existing = find_installed_state_entry(
        transaction,
        environment.dbi,
        package.system,
        package.name,
        package.version
    );
    if (!existing.has_value()) {
        mdb_txn_abort(transaction);
        return false;
    }

    std::set<std::string> removedOwners(ownerIds.begin(), ownerIds.end());
    InstalledEntry updated = existing->second;
    updated.owners.erase(std::remove_if(updated.owners.begin(), updated.owners.end(), [&](const std::string& owner) {
        return removedOwners.contains(owner);
    }), updated.owners.end());
    updated.owners = normalize_owners(std::move(updated.owners));
    updated.installMethod = install_method_from_owners(updated.owners);

    if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
        !put_value(transaction, environment.dbi, existing->first, serialize_installed_entry(updated))) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// HistoryManager
// ─────────────────────────────────────────────────────────────────────────────

HistoryManager::HistoryManager(const ReqPackConfig& cfg) : config(cfg) {}

std::filesystem::path HistoryManager::historyDir() const {
    if (!config.history.historyPath.empty()) {
        return std::filesystem::path(config.history.historyPath);
    }
    return default_reqpack_history_path();
}

std::filesystem::path HistoryManager::historyLogPath() const {
    return historyDir() / "history.jsonl";
}

std::filesystem::path HistoryManager::legacyInstalledStatePath() const {
    return historyDir() / "installed.json";
}

std::filesystem::path HistoryManager::installedStateDatabasePath() const {
    return historyDir();
}

bool HistoryManager::ensureDirectory() const {
    const std::filesystem::path dir = historyDir();
    if (std::filesystem::exists(dir)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string HistoryManager::escapeJson(const std::string& s) {
    return escape_json(s);
}

// Format:
//   {"timestamp":"…","action":"…","manager":"…","package":"…","version":"…","status":"…"}
// The "error" field is only included when there is an actual error message,
// keeping successful entries lean.
std::string HistoryManager::entryToJsonLine(const HistoryEntry& e) {
    std::string line;
    line.reserve(200);
    line += '{';
    line += jstr("timestamp", e.timestamp); line += ',';
    line += jstr("action",    e.action);    line += ',';
    line += jstr("manager",   e.system);    line += ',';
    line += jstr("package",   e.packageName); line += ',';
    line += jstr("version",   e.packageVersion); line += ',';
    line += jstr("status",    e.status);
    if (!e.errorMessage.empty()) {
        line += ',';
        line += jstr("error", e.errorMessage);
    }
    line += '}';
    return line;
}

// ── Rotation / trimming ───────────────────────────────────────────────────────

void HistoryManager::trimHistoryLog() const {
    const std::size_t maxLines  = config.history.maxLines;
    const double      maxSizeMb = config.history.maxSizeMb;

    if (maxLines == 0 && maxSizeMb <= 0.0) {
        return; // both limits disabled
    }

    const std::filesystem::path path = historyLogPath();
    if (!std::filesystem::exists(path)) {
        return;
    }

    // Check whether any limit is actually breached before loading everything.
    bool breached = false;
    if (maxSizeMb > 0.0) {
        std::error_code ec;
        const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
        if (!ec) {
            const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
            if (mb > maxSizeMb) {
                breached = true;
            }
        }
    }

    std::vector<std::string> lines;
    if (maxLines > 0 && !breached) {
        lines = read_all_lines(path);
        if (lines.size() > maxLines) {
            breached = true;
        }
    }

    if (!breached) {
        return;
    }

    // Load lines if not already loaded (size-based trigger).
    if (lines.empty()) {
        lines = read_all_lines(path);
    }
    if (lines.empty()) {
        return;
    }

    // Determine how many lines to keep.
    // Use the stricter of the two active limits.
    std::size_t keepCount = lines.size(); // start: keep all

    if (maxLines > 0 && keepCount > maxLines) {
        keepCount = maxLines;
    }

    if (maxSizeMb > 0.0) {
        // Estimate: walk from the end and accumulate sizes until we exceed the limit.
        const std::size_t maxBytes = static_cast<std::size_t>(maxSizeMb * 1024.0 * 1024.0);
        std::size_t total = 0;
        std::size_t sizeKeep = 0;
        for (std::size_t i = lines.size(); i > 0; --i) {
            total += lines[i - 1].size() + 1; // +1 for '\n'
            if (total <= maxBytes) {
                sizeKeep = lines.size() - (i - 1);
            } else {
                break;
            }
        }
        if (sizeKeep < keepCount) {
            keepCount = sizeKeep;
        }
    }

    if (keepCount >= lines.size()) {
        return; // nothing to drop
    }

    // Keep the newest entries (tail of the vector).
    const std::size_t dropCount = lines.size() - keepCount;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(dropCount));

    (void)write_all_lines(path, lines);
}

// ── Installed-state storage ───────────────────────────────────────────────────

std::vector<InstalledEntry> HistoryManager::loadInstalledState() const {
    std::lock_guard<std::mutex> lock(mutex);
    return load_installed_state_database(installedStateDatabasePath(), legacyInstalledStatePath());
}

// ── Public API ────────────────────────────────────────────────────────────────

bool HistoryManager::appendEvent(const HistoryEntry& entry) const {
    // Guard: only write the event log when history.enabled is true.
    if (!config.history.enabled) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!ensureDirectory()) {
        return false;
    }

    {
        std::ofstream file(historyLogPath(), std::ios::app);
        if (!file.is_open()) {
            return false;
        }
        file << entryToJsonLine(entry) << '\n';
        if (!file.good()) {
            return false;
        }
    }

    trimHistoryLog();
    return true;
}

bool HistoryManager::updateInstalledState(const HistoryEntry& entry) const {
    // Guard: only update the snapshot when history.trackInstalled is true.
    if (!config.history.trackInstalled) {
        return true;
    }
    // Only state-changing actions that succeeded affect the snapshot.
    if (entry.status != "success") {
        return true;
    }

    const bool isInstall = entry.action == "install" || entry.action == "ensure";
    const bool isRemove  = entry.action == "remove";
    const bool isUpdate  = entry.action == "update";

    if (!isInstall && !isRemove && !isUpdate) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (isRemove) {
        return remove_installed_state_database(installedStateDatabasePath(), legacyInstalledStatePath(), entry.system, entry.packageName, entry.packageVersion);
    }

    if (isUpdate && !remove_installed_state_database(installedStateDatabasePath(), legacyInstalledStatePath(), entry.system, entry.packageName, {})) {
        return false;
    }

    return upsert_installed_state_database(
        installedStateDatabasePath(),
        legacyInstalledStatePath(),
        InstalledEntry{
            .name        = entry.packageName,
            .version     = entry.packageVersion,
            .system      = entry.system,
            .installedAt = entry.timestamp
        }
    );
}

bool HistoryManager::replaceInstalledState(const std::string& system, const std::vector<InstalledEntry>& entries) const {
    if (!config.history.trackInstalled) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    return replace_installed_state_database(installedStateDatabasePath(), legacyInstalledStatePath(), system, entries);
}

bool HistoryManager::mergeInstalledOwnership(const Package& package, const std::vector<std::string>& ownerIds, bool directRequest) const {
    if (!config.history.trackInstalled) {
        return true;
    }
    if (package.system.empty() || package.name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    return merge_installed_ownership_database(installedStateDatabasePath(), legacyInstalledStatePath(), package, ownerIds, directRequest);
}

bool HistoryManager::subtractInstalledOwnership(const Package& package, const std::vector<std::string>& ownerIds) const {
    if (!config.history.trackInstalled) {
        return true;
    }
    if (package.system.empty() || package.name.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    return subtract_installed_ownership_database(installedStateDatabasePath(), legacyInstalledStatePath(), package, ownerIds);
}

bool HistoryManager::record(const HistoryEntry& entry) const {
    // Inject timestamp if caller left it empty.
    HistoryEntry filled = entry;
    if (filled.timestamp.empty()) {
        filled.timestamp = utc_timestamp_now();
    }

    // appendEvent and updateInstalledState each check their own flag internally.
    bool ok = appendEvent(filled);
    ok = updateInstalledState(filled) && ok;
    return ok;
}
