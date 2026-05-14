#include "transaction_database_internal.h"
#include "core/state/transaction_database_core.h"

#include <chrono>

namespace transaction_database_internal {

std::string now_timestamp() {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace transaction_database_internal

std::string TransactionDatabase::createRun(const std::vector<Package>& packages, const std::vector<std::string>& flags) const {
    if (!this->ensureReady()) {
        return {};
    }

    if (const auto activeRun = this->loadString(std::string(transaction_database_internal::ACTIVE_RUN_KEY)); activeRun.has_value() && !activeRun->empty()) {
        return {};
    }

    const std::string timestamp = transaction_database_internal::now_timestamp();
    const std::string runId = timestamp + "-" + std::to_string(packages.size());
    TransactionRunRecord run{.id = runId, .state = "open", .createdAt = timestamp, .updatedAt = timestamp, .flags = flags};

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return {};
    }

    if (!transaction_database_internal::put_value(transaction, this->dbi, std::string(transaction_database_internal::ACTIVE_RUN_KEY), runId) ||
        !transaction_database_internal::put_value(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(run))) {
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
        if (!transaction_database_internal::put_value(transaction, this->dbi, transaction_database_item_key(runId, package), transaction_database_serialize_item(item))) {
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
        if (!transaction_database_internal::load_value(transaction, this->dbi, key, payload)) {
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
        if (!transaction_database_internal::put_value(transaction, this->dbi, key, transaction_database_serialize_item(updated))) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    std::string runPayload;
    if (transaction_database_internal::load_value(transaction, this->dbi, transaction_database_run_key(runId), runPayload)) {
        if (const auto run = transaction_database_deserialize_run(runId, runPayload)) {
            TransactionRunRecord updatedRun = run.value();
            updatedRun.updatedAt = transaction_database_internal::now_timestamp();
            if (!transaction_database_internal::put_value(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(updatedRun))) {
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
    if (!transaction_database_internal::load_value(transaction, this->dbi, transaction_database_run_key(runId), payload)) {
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
    updatedRun.updatedAt = transaction_database_internal::now_timestamp();
    if (!transaction_database_internal::put_value(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(updatedRun))) {
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
        if (!transaction_database_internal::put_value(transaction, this->dbi, transaction_database_item_key(runId, item.package), transaction_database_serialize_item(updatedItem))) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    const std::string timestamp = transaction_database_internal::now_timestamp();
    TransactionRunRecord run{.id = runId, .state = "committed", .createdAt = timestamp, .updatedAt = timestamp};
    std::string runPayload;
    if (transaction_database_internal::load_value(transaction, this->dbi, transaction_database_run_key(runId), runPayload)) {
        if (const auto existingRun = transaction_database_deserialize_run(runId, runPayload)) {
            run = existingRun.value();
            run.state = "committed";
            run.updatedAt = transaction_database_internal::now_timestamp();
        }
    }

    if (!transaction_database_internal::put_value(transaction, this->dbi, transaction_database_run_key(runId), transaction_database_serialize_run(run))) {
        mdb_txn_abort(transaction);
        return false;
    }

    std::string activeRunId;
    if (transaction_database_internal::load_value(transaction, this->dbi, std::string(transaction_database_internal::ACTIVE_RUN_KEY), activeRunId) && activeRunId == runId) {
        (void)transaction_database_internal::delete_value(transaction, this->dbi, std::string(transaction_database_internal::ACTIVE_RUN_KEY));
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
        if (!transaction_database_internal::delete_value(transaction, this->dbi, key)) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    if (!transaction_database_internal::delete_value(transaction, this->dbi, transaction_database_run_key(runId))) {
        mdb_txn_abort(transaction);
        return false;
    }

    std::string activeRunId;
    if (transaction_database_internal::load_value(transaction, this->dbi, std::string(transaction_database_internal::ACTIVE_RUN_KEY), activeRunId) && activeRunId == runId) {
        (void)transaction_database_internal::delete_value(transaction, this->dbi, std::string(transaction_database_internal::ACTIVE_RUN_KEY));
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}
