#include "history_manager_internal.h"

#include <lmdb.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr unsigned int INSTALLED_STATE_MAX_DATABASES = 1;
constexpr std::size_t INSTALLED_STATE_MAP_SIZE = 8 * 1024 * 1024;
constexpr std::string_view INSTALLED_STATE_KEY_PREFIX = "pkg:";
constexpr std::string_view INSTALLED_STATE_INITIALIZED_KEY = "meta:initialized";

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

InstalledStateEnvironment open_installed_state_environment(const std::filesystem::path& dbDirectory, bool createIfMissing) {
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
		if (!history_manager_internal::starts_with(currentKey, prefix)) {
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

std::vector<InstalledEntry> load_entries_for_keys(MDB_txn* transaction, MDB_dbi dbi, const std::vector<std::string>& keys) {
	std::vector<InstalledEntry> entries;
	entries.reserve(keys.size());
	for (const std::string& key : keys) {
		const std::optional<std::string> payload = read_value(transaction, dbi, key);
		if (!payload.has_value()) {
			continue;
		}
		if (const std::optional<InstalledEntry> entry = history_manager_internal::deserialize_installed_entry(payload.value())) {
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
	const std::vector<std::string> keys = collect_keys_with_prefix(transaction, dbi, history_manager_internal::installed_state_name_prefix(system, name));
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
	const auto identity = existingByIdentity.find(history_manager_internal::installed_state_identity(candidate));
	if (identity != existingByIdentity.end()) {
		return identity->second;
	}

	const auto sameName = existingByName.find(candidate.name);
	if (sameName != existingByName.end() && sameName->second.size() == 1) {
		return sameName->second.front();
	}

	return std::nullopt;
}

bool ensure_legacy_installed_state_imported(const InstalledStateEnvironment& environment, const std::filesystem::path& legacyPath) {
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

	const std::vector<InstalledEntry> legacyEntries = history_manager_internal::read_legacy_installed_state(legacyPath);
	for (const InstalledEntry& entry : legacyEntries) {
		if (!put_value(transaction, environment.dbi, history_manager_internal::installed_state_key(entry.system, entry.name, entry.version), history_manager_internal::serialize_installed_entry(entry))) {
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
		if (history_manager_internal::starts_with(keyString, INSTALLED_STATE_KEY_PREFIX)) {
			const std::string payload(static_cast<const char*>(value.mv_data), value.mv_size);
			if (const std::optional<InstalledEntry> entry = history_manager_internal::deserialize_installed_entry(payload)) {
				entries.push_back(entry.value());
			}
		}

		result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
	}

	mdb_cursor_close(cursor);
	mdb_txn_abort(transaction);
	return entries;
}

std::vector<InstalledEntry> load_installed_state_database(const std::filesystem::path& dbDirectory, const std::filesystem::path& legacyPath) {
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
	    !put_value(transaction, environment.dbi, history_manager_internal::installed_state_key(entry.system, entry.name, entry.version), history_manager_internal::serialize_installed_entry(entry))) {
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
		? collect_keys_with_prefix(transaction, environment.dbi, history_manager_internal::installed_state_name_prefix(system, name))
		: std::vector<std::string>{history_manager_internal::installed_state_key(system, name, version)};

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

	const std::vector<std::string> existingKeys = collect_keys_with_prefix(transaction, environment.dbi, history_manager_internal::installed_state_system_prefix(system));
	std::map<std::string, InstalledEntry> existingByIdentity;
	std::map<std::string, std::vector<InstalledEntry>> existingByName;
	for (const std::string& key : existingKeys) {
		const std::optional<std::string> payload = read_value(transaction, environment.dbi, key);
		if (!payload.has_value()) {
			continue;
		}
		if (const auto entry = history_manager_internal::deserialize_installed_entry(payload.value())) {
			existingByIdentity[history_manager_internal::installed_state_identity(entry.value())] = entry.value();
			existingByName[entry->name].push_back(entry.value());
		}
	}

	if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
	    !delete_keys(transaction, environment.dbi, existingKeys)) {
		mdb_txn_abort(transaction);
		return false;
	}

	const std::string syncedAt = history_manager_internal::utc_timestamp_now();
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
		entry.owners = history_manager_internal::normalize_owners(std::move(entry.owners));
		if (entry.installMethod.empty() || entry.installMethod == "unknown") {
			entry.installMethod = existing.has_value() && !existing->installMethod.empty()
				? existing->installMethod
				: history_manager_internal::install_method_from_owners(entry.owners);
		}

		if (!put_value(transaction, environment.dbi, history_manager_internal::installed_state_key(entry.system, entry.name, entry.version), history_manager_internal::serialize_installed_entry(entry))) {
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
	merged.owners = history_manager_internal::normalize_owners(std::move(merged.owners));
	merged.installMethod = history_manager_internal::install_method_from_owners(merged.owners);
	if (directRequest && merged.installMethod == "unknown") {
		merged.installMethod = "explicit";
	}

	if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
	    !put_value(transaction, environment.dbi, existing->first, history_manager_internal::serialize_installed_entry(merged))) {
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
	updated.owners = history_manager_internal::normalize_owners(std::move(updated.owners));
	updated.installMethod = history_manager_internal::install_method_from_owners(updated.owners);

	if (!put_value(transaction, environment.dbi, std::string(INSTALLED_STATE_INITIALIZED_KEY), "1") ||
	    !put_value(transaction, environment.dbi, existing->first, history_manager_internal::serialize_installed_entry(updated))) {
		mdb_txn_abort(transaction);
		return false;
	}

	return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

} // namespace

std::vector<InstalledEntry> HistoryManager::loadInstalledState() const {
	std::lock_guard<std::mutex> lock(mutex);
	return load_installed_state_database(installedStateDatabasePath(), legacyInstalledStatePath());
}

bool HistoryManager::updateInstalledState(const HistoryEntry& entry) const {
	if (!config.history.trackInstalled) {
		return true;
	}
	if (entry.status != "success") {
		return true;
	}

	const bool isInstall = entry.action == "install" || entry.action == "ensure";
	const bool isRemove = entry.action == "remove";
	const bool isUpdate = entry.action == "update";

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
			.name = entry.packageName,
			.version = entry.packageVersion,
			.system = entry.system,
			.installedAt = entry.timestamp,
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
