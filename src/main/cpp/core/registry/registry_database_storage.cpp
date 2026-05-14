#include "registry_database_internal.h"

#include <filesystem>

std::optional<RegistryRecord> get_record_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& name) {
    const std::string normalized = registry_database_to_lower_copy(name);
    MDB_val key{normalized.size(), const_cast<char*>(normalized.data())};
    MDB_val value;
    if (mdb_get(transaction, database, &key, &value) != MDB_SUCCESS) {
        return std::nullopt;
    }

    return registry_database_deserialize_record(normalized, std::string(static_cast<const char*>(value.mv_data), value.mv_size));
}

bool put_record_into_transaction(MDB_txn* transaction, MDB_dbi database, const RegistryRecord& record) {
    const std::string payload = registry_database_serialize_record(record);
    const std::string normalizedName = registry_database_to_lower_copy(record.name);
    MDB_val key{normalizedName.size(), const_cast<char*>(normalizedName.data())};
    MDB_val value{payload.size(), const_cast<char*>(payload.data())};
    return mdb_put(transaction, database, &key, &value, 0) == MDB_SUCCESS;
}

bool delete_record_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& name) {
    const std::string normalizedName = registry_database_to_lower_copy(name);
    MDB_val key{normalizedName.size(), const_cast<char*>(normalizedName.data())};
    const int result = mdb_del(transaction, database, &key, nullptr);
    return result == MDB_SUCCESS || result == MDB_NOTFOUND;
}

std::optional<std::string> get_string_from_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& key) {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val value;
    if (mdb_get(transaction, database, &keyValue, &value) != MDB_SUCCESS) {
        return std::nullopt;
    }

    return std::string(static_cast<const char*>(value.mv_data), value.mv_size);
}

bool put_string_into_transaction(MDB_txn* transaction, MDB_dbi database, const std::string& key, const std::string& value) {
    MDB_val keyValue{key.size(), const_cast<char*>(key.data())};
    MDB_val storedValue{value.size(), const_cast<char*>(value.data())};
    return mdb_put(transaction, database, &keyValue, &storedValue, 0) == MDB_SUCCESS;
}

bool put_meta_values_into_transaction(MDB_txn* transaction, MDB_dbi database, const std::map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) {
        if (!put_string_into_transaction(transaction, database, key, value)) {
            return false;
        }
    }
    return true;
}

RegistryDatabase::~RegistryDatabase() {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->initialized && this->env != nullptr) {
        if (this->metaDbi != 0) {
            mdb_dbi_close(this->env, this->metaDbi);
            this->metaDbi = 0;
        }
        if (this->dbi != 0) {
            mdb_dbi_close(this->env, this->dbi);
            this->dbi = 0;
        }
        mdb_env_close(this->env);
        this->env = nullptr;
        this->initialized = false;
        this->bootstrapped = false;
    }
}

bool RegistryDatabase::initStorage() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->initialized) {
        return true;
    }

    const std::filesystem::path dbDirectory = registry_database_directory(this->config.registry.databasePath);
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

    if (mdb_dbi_open(transaction, REGISTRY_META_DATABASE_NAME, MDB_CREATE, &this->metaDbi) != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        mdb_env_close(this->env);
        this->env = nullptr;
        return false;
    }

    if (mdb_txn_commit(transaction) != MDB_SUCCESS) {
        if (this->metaDbi != 0) {
            mdb_dbi_close(this->env, this->metaDbi);
            this->metaDbi = 0;
        }
        if (this->dbi != 0) {
            mdb_dbi_close(this->env, this->dbi);
            this->dbi = 0;
        }
        mdb_env_close(this->env);
        this->env = nullptr;
        return false;
    }

    this->initialized = true;
    return true;
}

std::vector<RegistryRecord> RegistryDatabase::load_all_records() const {
    std::vector<RegistryRecord> records;
    if (!this->initialized) {
        return records;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    MDB_cursor* cursor = nullptr;
    if (mdb_txn_begin(this->env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
        return records;
    }

    if (mdb_cursor_open(transaction, this->dbi, &cursor) != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        return records;
    }

    MDB_val key;
    MDB_val value;
    int result = mdb_cursor_get(cursor, &key, &value, MDB_FIRST);
    while (result == MDB_SUCCESS) {
        const std::string name(static_cast<const char*>(key.mv_data), key.mv_size);
        const std::string payload(static_cast<const char*>(value.mv_data), value.mv_size);
        if (const std::optional<RegistryRecord> record = registry_database_deserialize_record(name, payload)) {
            records.push_back(record.value());
        }

        result = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(transaction);
    return records;
}

std::optional<std::string> RegistryDatabase::load_meta_value(const std::string& key) const {
    if (key.empty() || !this->initialized) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
        return std::nullopt;
    }

    const std::optional<std::string> value = get_string_from_transaction(transaction, this->metaDbi, key);
    mdb_txn_abort(transaction);
    return value;
}

std::optional<std::string> RegistryDatabase::getMetaValue(const std::string& key) const {
    if (key.empty() || !this->ensureReady()) {
        return std::nullopt;
    }
    return this->load_meta_value(key);
}

bool RegistryDatabase::putMetaValue(const std::string& key, const std::string& value) const {
    if (key.empty() || !this->ensureReady()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    if (!put_string_into_transaction(transaction, this->metaDbi, key, value)) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

std::optional<RegistryRecord> RegistryDatabase::load_record(const std::string& name) const {
    if (!this->initialized) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, MDB_RDONLY, &transaction) != MDB_SUCCESS) {
        return std::nullopt;
    }

    const std::optional<RegistryRecord> record = get_record_from_transaction(transaction, this->dbi, name);
    mdb_txn_abort(transaction);
    return record;
}

bool RegistryDatabase::put_record(const RegistryRecord& record) const {
    if (!this->initialized) {
        return false;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    if (!put_record_into_transaction(transaction, this->dbi, record)) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}
