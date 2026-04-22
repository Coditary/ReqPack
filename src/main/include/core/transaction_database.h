#pragma once

#include "core/configuration.h"
#include "core/types.h"

#include <lmdb.h>

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct TransactionRunRecord {
	std::string id;
	std::string state;
	std::string createdAt;
	std::string updatedAt;
};

struct TransactionItemRecord {
	std::string runId;
	std::string itemId;
	std::size_t sequence{0};
	Package package;
	std::string status;
	std::string errorMessage;
};

class TransactionDatabase {
	ReqPackConfig config;
	mutable std::mutex mutex;
	mutable MDB_env* env{nullptr};
	mutable MDB_dbi dbi{0};
	mutable bool initialized{false};

	bool initStorage() const;
	std::optional<std::string> loadString(const std::string& key) const;
	std::vector<std::pair<std::string, std::string>> loadPrefixedEntries(const std::string& prefix) const;

public:
	TransactionDatabase(const ReqPackConfig& config = DEFAULT_REQPACK_CONFIG);
	~TransactionDatabase();

	bool ensureReady() const;
	std::optional<TransactionRunRecord> getActiveRun() const;
	std::vector<TransactionItemRecord> getRunItems(const std::string& runId) const;
	std::string createRun(const std::vector<Package>& packages) const;
	bool updateItemStatus(const std::string& runId, const Package& package, const std::string& status, const std::string& errorMessage = {}) const;
	bool updateItemsStatus(const std::string& runId, const std::vector<Package>& packages, const std::string& status, const std::string& errorMessage = {}) const;
	bool markRunState(const std::string& runId, const std::string& state) const;
	bool markRunCommitted(const std::string& runId) const;
	bool deleteRun(const std::string& runId) const;
};
