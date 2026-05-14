#include "transaction_database_internal.h"
#include "core/state/transaction_database_core.h"

#include <algorithm>

std::optional<TransactionRunRecord> TransactionDatabase::getActiveRun() const {
    const std::optional<std::string> activeRunId = this->loadString(std::string(transaction_database_internal::ACTIVE_RUN_KEY));
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
