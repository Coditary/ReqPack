#include "core/state/transaction_database.h"
#include "core/state/transaction_database_core.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

namespace {

constexpr unsigned int LMDB_MAX_DATABASES = 1;
constexpr std::size_t LMDB_MAP_SIZE = 8 * 1024 * 1024;
constexpr std::string_view ACTIVE_RUN_KEY = "meta:active_run";

bool startsWith(const std::string& value, std::string_view prefix) {
	return value.rfind(prefix, 0) == 0;
}

std::string nowTimestamp() {
	using Clock = std::chrono::system_clock;
	const auto now = Clock::now().time_since_epoch();
	return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::uint64_t fnv1aHash(std::string_view value) {
	std::uint64_t hash = 14695981039346656037ull;
	for (unsigned char c : value) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return hash;
}

bool loadValue(MDB_txn* transaction, MDB_dbi database, const std::string& key, std::string& value) {
	MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
	MDB_val dbValue;
	if (mdb_get(transaction, database, &dbKey, &dbValue) != MDB_SUCCESS) {
		return false;
	}
	value.assign(static_cast<const char*>(dbValue.mv_data), dbValue.mv_size);
	return true;
}

bool putValue(MDB_txn* transaction, MDB_dbi database, const std::string& key, const std::string& value) {
	MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
	MDB_val dbValue{value.size(), const_cast<char*>(value.data())};
	return mdb_put(transaction, database, &dbKey, &dbValue, 0) == MDB_SUCCESS;
}

bool deleteValue(MDB_txn* transaction, MDB_dbi database, const std::string& key) {
	MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
	return mdb_del(transaction, database, &dbKey, nullptr) == MDB_SUCCESS;
}

}  // namespace

TransactionDatabase::TransactionDatabase(const ReqPackConfig& config) : config(config) {}

TransactionDatabase::~TransactionDatabase() {
	std::lock_guard<std::mutex> lock(this->mutex);
	if (this->initialized && this->env != nullptr) {
		mdb_dbi_close(this->env, this->dbi);
		mdb_env_close(this->env);
		this->env = nullptr;
		this->initialized = false;
	}
}

bool TransactionDatabase::ensureReady() const {
	return this->initStorage();
}

bool TransactionDatabase::initStorage() const {
	std::lock_guard<std::mutex> lock(this->mutex);
	if (this->initialized) {
		return true;
	}

	const std::filesystem::path dbDirectory(this->config.execution.transactionDatabasePath);
	std::error_code directoryError;
	std::filesystem::create_directories(dbDirectory, directoryError);
	if (directoryError) {
		return false;
	}

	if (mdb_env_create(&this->env) != MDB_SUCCESS) {
		this->env = nullptr;
		return false;
	}

	mdb_env_set_maxdbs(this->env, LMDB_MAX_DATABASES);
	mdb_env_set_mapsize(this->env, LMDB_MAP_SIZE);
	if (mdb_env_open(this->env, dbDirectory.string().c_str(), 0, 0664) != MDB_SUCCESS) {
		mdb_env_close(this->env);
		this->env = nullptr;
		return false;
	}

	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		mdb_env_close(this->env);
		this->env = nullptr;
		return false;
	}

	if (mdb_dbi_open(transaction, nullptr, MDB_CREATE, &this->dbi) != MDB_SUCCESS) {
		mdb_txn_abort(transaction);
		mdb_env_close(this->env);
		this->env = nullptr;
		return false;
	}

	if (mdb_txn_commit(transaction) != MDB_SUCCESS) {
		mdb_dbi_close(this->env, this->dbi);
		mdb_env_close(this->env);
		this->env = nullptr;
		return false;
	}

	this->initialized = true;
	return true;
}

std::optional<std::string> TransactionDatabase::loadString(const std::string& key) const {
	if (!this->ensureReady()) {
		return std::nullopt;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
		return std::nullopt;
	}

	std::string value;
	const bool loaded = loadValue(transaction, this->dbi, key, value);
	mdb_txn_abort(transaction);
	if (!loaded) {
		return std::nullopt;
	}
	return value;
}

std::vector<std::pair<std::string, std::string>> TransactionDatabase::loadPrefixedEntries(const std::string& prefix) const {
	std::vector<std::pair<std::string, std::string>> entries;
	if (!this->ensureReady()) {
		return entries;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	MDB_cursor* cursor = nullptr;
	if (mdb_txn_begin(this->env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
		return entries;
	}

	if (mdb_cursor_open(transaction, this->dbi, &cursor) != MDB_SUCCESS) {
		mdb_txn_abort(transaction);
		return entries;
	}

	MDB_val key{prefix.size(), const_cast<char*>(prefix.data())};
	MDB_val value;
	int result = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
	while (result == MDB_SUCCESS) {
		const std::string currentKey(static_cast<const char*>(key.mv_data), key.mv_size);
		if (!startsWith(currentKey, prefix)) {
			break;
		}
		entries.emplace_back(currentKey, std::string(static_cast<const char*>(value.mv_data), value.mv_size));
		result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
	}

	mdb_cursor_close(cursor);
	mdb_txn_abort(transaction);
	return entries;
}

std::optional<TransactionRunRecord> TransactionDatabase::getActiveRun() const {
	const std::optional<std::string> activeRunId = this->loadString(std::string(ACTIVE_RUN_KEY));
	if (!activeRunId.has_value() || activeRunId->empty()) {
		return std::nullopt;
	}

	const std::optional<std::string> payload = this->loadString(transaction_database_run_key(activeRunId.value()));
	if (!payload.has_value()) {
		return std::nullopt;
	}

	return transaction_database_deserialize_run(activeRunId.value(), payload.value());
}

std::vector<TransactionItemRecord> TransactionDatabase::getRunItems(const std::string& runId) const {
	std::vector<TransactionItemRecord> items;
	for (const auto& [key, value] : this->loadPrefixedEntries(transaction_database_item_prefix(runId))) {
		if (const auto item = transaction_database_deserialize_item(key, value)) {
			items.push_back(item.value());
		}
	}

	std::sort(items.begin(), items.end(), [](const TransactionItemRecord& left, const TransactionItemRecord& right) {
		return left.sequence < right.sequence;
	});
	return items;
}

std::string TransactionDatabase::createRun(const std::vector<Package>& packages, const std::vector<std::string>& flags) const {
	if (!this->ensureReady()) {
		return {};
	}

	if (const auto activeRun = this->loadString(std::string(ACTIVE_RUN_KEY)); activeRun.has_value() && !activeRun->empty()) {
		return {};
	}

	const std::string timestamp = nowTimestamp();
	const std::string runId = timestamp + "-" + std::to_string(packages.size());
	TransactionRunRecord run{.id = runId, .state = "open", .createdAt = timestamp, .updatedAt = timestamp, .flags = flags};

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		return {};
	}

	if (!putValue(transaction, this->dbi, std::string(ACTIVE_RUN_KEY), runId) ||
		!putValue(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(run))) {
		mdb_txn_abort(transaction);
		return {};
	}

	for (std::size_t index = 0; index < packages.size(); ++index) {
		const Package& package = packages[index];
		TransactionItemRecord item;
		item.runId = runId;
		item.itemId = transaction_database_item_id_for_package(package);
		item.sequence = index;
		item.package = package;
		item.status = "planned";
		if (!putValue(transaction, this->dbi, transaction_database_item_key(runId, package), transaction_database_serialize_item(item))) {
			mdb_txn_abort(transaction);
			return {};
		}
	}

	if (mdb_txn_commit(transaction) != MDB_SUCCESS) {
		return {};
	}

	return runId;
}

bool TransactionDatabase::updateItemStatus(const std::string& runId, const Package& package, const std::string& status, const std::string& errorMessage) const {
	return this->updateItemsStatus(runId, {package}, status, errorMessage);
}

bool TransactionDatabase::updateItemsStatus(const std::string& runId, const std::vector<Package>& packages, const std::string& status, const std::string& errorMessage) const {
	if (!this->ensureReady()) {
		return false;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		return false;
	}

	for (const Package& package : packages) {
		const std::string key = transaction_database_item_key(runId, package);
		std::string payload;
		if (!loadValue(transaction, this->dbi, key, payload)) {
			mdb_txn_abort(transaction);
			return false;
		}

		const std::optional<TransactionItemRecord> item = transaction_database_deserialize_item(key, payload);
		if (!item.has_value()) {
			mdb_txn_abort(transaction);
			return false;
		}

		TransactionItemRecord updated = item.value();
		updated.status = status;
		updated.errorMessage = errorMessage;
		if (!putValue(transaction, this->dbi, key, transaction_database_serialize_item(updated))) {
			mdb_txn_abort(transaction);
			return false;
		}
	}

	std::string runPayload;
	if (loadValue(transaction, this->dbi, transaction_database_run_key(runId), runPayload)) {
		if (const auto run = transaction_database_deserialize_run(runId, runPayload)) {
			TransactionRunRecord updatedRun = run.value();
			updatedRun.updatedAt = nowTimestamp();
			if (!putValue(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(updatedRun))) {
				mdb_txn_abort(transaction);
				return false;
			}
		}
	}

	return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool TransactionDatabase::markRunState(const std::string& runId, const std::string& state) const {
	if (!this->ensureReady()) {
		return false;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		return false;
	}

	std::string payload;
	if (!loadValue(transaction, this->dbi, transaction_database_run_key(runId), payload)) {
		mdb_txn_abort(transaction);
		return false;
	}

	const std::optional<TransactionRunRecord> run = transaction_database_deserialize_run(runId, payload);
	if (!run.has_value()) {
		mdb_txn_abort(transaction);
		return false;
	}

	TransactionRunRecord updatedRun = run.value();
	updatedRun.state = state;
	updatedRun.updatedAt = nowTimestamp();
	if (!putValue(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(updatedRun))) {
		mdb_txn_abort(transaction);
		return false;
	}

	return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool TransactionDatabase::markRunCommitted(const std::string& runId) const {
	const std::vector<TransactionItemRecord> items = this->getRunItems(runId);
	if (!this->ensureReady()) {
		return false;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		return false;
	}

	for (const TransactionItemRecord& item : items) {
		TransactionItemRecord updatedItem = item;
		updatedItem.status = "committed";
		updatedItem.errorMessage.clear();
		if (!putValue(transaction, this->dbi, transaction_database_item_key(runId, item.package), transaction_database_serialize_item(updatedItem))) {
			mdb_txn_abort(transaction);
			return false;
		}
	}

	TransactionRunRecord run{.id = runId, .state = "committed", .createdAt = nowTimestamp(), .updatedAt = nowTimestamp()};
	std::string runPayload;
	if (loadValue(transaction, this->dbi, transaction_database_run_key(runId), runPayload)) {
		if (const auto existingRun = transaction_database_deserialize_run(runId, runPayload)) {
			run = existingRun.value();
			run.state = "committed";
			run.updatedAt = nowTimestamp();
		}
	}

	if (!putValue(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(run))) {
		mdb_txn_abort(transaction);
		return false;
	}

	std::string activeRunId;
	if (loadValue(transaction, this->dbi, std::string(ACTIVE_RUN_KEY), activeRunId) && activeRunId == runId) {
		(void)deleteValue(transaction, this->dbi, std::string(ACTIVE_RUN_KEY));
	}

	return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool TransactionDatabase::deleteRun(const std::string& runId) const {
	const std::vector<std::pair<std::string, std::string>> entries = this->loadPrefixedEntries(transaction_database_item_prefix(runId));
	if (!this->ensureReady()) {
		return false;
	}

	std::lock_guard<std::mutex> lock(this->mutex);
	MDB_txn* transaction = nullptr;
	if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
		return false;
	}

	for (const auto& [key, _] : entries) {
		if (!deleteValue(transaction, this->dbi, key)) {
			mdb_txn_abort(transaction);
			return false;
		}
	}

	if (!deleteValue(transaction, this->dbi, transaction_database_run_key(runId))) {
		mdb_txn_abort(transaction);
		return false;
	}

	std::string activeRunId;
	if (loadValue(transaction, this->dbi, std::string(ACTIVE_RUN_KEY), activeRunId) && activeRunId == runId) {
		(void)deleteValue(transaction, this->dbi, std::string(ACTIVE_RUN_KEY));
	}

	return mdb_txn_commit(transaction) == MDB_SUCCESS;
}
