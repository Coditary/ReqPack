#pragma once

#include "core/state/transaction_database.h"

#include <string_view>

namespace transaction_database_internal {

constexpr unsigned int LMDB_MAX_DATABASES = 1;
constexpr std::size_t LMDB_MAP_SIZE = 8 * 1024 * 1024;
constexpr std::string_view ACTIVE_RUN_KEY = "meta:active_run";

bool starts_with(const std::string& value, std::string_view prefix);
std::string now_timestamp();

bool load_value(MDB_txn* transaction, MDB_dbi database, const std::string& key, std::string& value);
bool put_value(MDB_txn* transaction, MDB_dbi database, const std::string& key, const std::string& value);
bool delete_value(MDB_txn* transaction, MDB_dbi database, const std::string& key);

}  // namespace transaction_database_internal
