#include "transaction_database_internal.h"
#include "core/state/transaction_database_core.h"

#include <filesystem>

namespace transaction_database_internal {

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool load_value(MDB_txn* transaction, MDB_dbi database, const std::string& key, std::string& value) {
    MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
    MDB_val dbValue;
    if (mdb_get(transaction, database, &dbKey, &dbValue) != MDB_SUCCESS) {
        return false;
    }
    value.assign(static_cast<const char*>(dbValue.mv_data), dbValue.mv_size);
    return true;
}

bool put_value(MDB_txn* transaction, MDB_dbi database, const std::string& key, const std::string& value) {
    MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
    MDB_val dbValue{value.size(), const_cast<char*>(value.data())};
    return mdb_put(transaction, database, &dbKey, &dbValue, 0) == MDB_SUCCESS;
}

bool delete_value(MDB_txn* transaction, MDB_dbi database, const std::string& key) {
    MDB_val dbKey{key.size(), const_cast<char*>(key.data())};
    return mdb_del(transaction, database, &dbKey, nullptr) == MDB_SUCCESS;
}

}  // namespace transaction_database_internal

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

    mdb_env_set_maxdbs(this->env, transaction_database_internal::LMDB_MAX_DATABASES);
    mdb_env_set_mapsize(this->env, transaction_database_internal::LMDB_MAP_SIZE);
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
    const bool loaded = transaction_database_internal::load_value(transaction, this->dbi, key, value);
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
        if (!transaction_database_internal::starts_with(currentKey, prefix)) {
            break;
        }
        entries.emplace_back(currentKey, std::string(static_cast<const char*>(value.mv_data), value.mv_size));
        result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(transaction);
    return entries;
}
