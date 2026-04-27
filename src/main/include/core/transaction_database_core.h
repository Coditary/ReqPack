#pragma once

#include "core/transaction_database.h"

#include <optional>
#include <string>

std::string transaction_database_package_token(const Package& package);
std::string transaction_database_item_id_for_package(const Package& package);
std::string transaction_database_run_key(const std::string& runId);
std::string transaction_database_item_prefix(const std::string& runId);
std::string transaction_database_item_key(const std::string& runId, const Package& package);
std::string transaction_database_escape_field(const std::string& value);
std::string transaction_database_unescape_field(const std::string& value);
std::string transaction_database_serialize_run(const TransactionRunRecord& run);
std::optional<TransactionRunRecord> transaction_database_deserialize_run(const std::string& runId, const std::string& payload);
std::string transaction_database_serialize_item(const TransactionItemRecord& item);
std::optional<TransactionItemRecord> transaction_database_deserialize_item(const std::string& key, const std::string& payload);
