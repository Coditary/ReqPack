#include "registry_database_internal.h"

#include <map>
#include <set>

namespace {

bool registry_records_equal(const RegistryRecord& left, const RegistryRecord& right) {
    return left.name == right.name &&
           left.source == right.source &&
           left.alias == right.alias &&
           left.originPath == right.originPath &&
           left.description == right.description &&
           left.role == right.role &&
           left.targetSystem == right.targetSystem &&
           left.capabilities == right.capabilities &&
           left.ecosystemScopes == right.ecosystemScopes &&
           left.writeScopes == right.writeScopes &&
           left.networkScopes == right.networkScopes &&
           left.privilegeLevel == right.privilegeLevel &&
           left.scriptSha256 == right.scriptSha256 &&
           left.bootstrapSha256 == right.bootstrapSha256 &&
           left.script == right.script &&
           left.bootstrapScript == right.bootstrapScript &&
           left.bundlePath == right.bundlePath &&
           left.bundleSource == right.bundleSource;
}

}  // namespace

bool RegistryDatabase::sync_records(
    const std::vector<RegistryRecord>& records,
    const bool fetchPayloads,
    const bool replaceMissing,
    const std::map<std::string, std::string>& metaValues,
    const std::vector<std::string>& originPathsToDelete
) const {
    if (!this->initialized) {
        return false;
    }

    std::map<std::string, RegistryRecord> desiredRecords;
    for (RegistryRecord record : records) {
        record.name = registry_database_to_lower_copy(record.name);

        if (const std::optional<RegistryRecord> existing = this->load_record(record.name)) {
            if (record.originPath.empty()) {
                record.originPath = existing->originPath;
            }
            if (record.description.empty()) {
                record.description = existing->description;
            }
            if (record.role.empty()) {
                record.role = existing->role;
            }
            if (record.targetSystem.empty()) {
                record.targetSystem = existing->targetSystem;
            }
            if (record.capabilities.empty()) {
                record.capabilities = existing->capabilities;
            }
            if (record.ecosystemScopes.empty()) {
                record.ecosystemScopes = existing->ecosystemScopes;
            }
            if (record.writeScopes.empty()) {
                record.writeScopes = existing->writeScopes;
            }
            if (record.networkScopes.empty()) {
                record.networkScopes = existing->networkScopes;
            }
            if (record.privilegeLevel.empty()) {
                record.privilegeLevel = existing->privilegeLevel;
            }
            if (record.scriptSha256.empty()) {
                record.scriptSha256 = existing->scriptSha256;
            }
            if (record.bootstrapSha256.empty()) {
                record.bootstrapSha256 = existing->bootstrapSha256;
            }

            const bool sourceChanged = existing->source != record.source || existing->alias != record.alias;
            if (!sourceChanged) {
                record.script = existing->script;
                record.bootstrapScript = existing->bootstrapScript;
                record.bundleSource = existing->bundleSource;
                record.bundlePath = existing->bundlePath;
            }
        }

        if (registry_record_can_materialize_plugin(record) && !record.bundleSource) {
            if (const auto bundlePath = resolve_bundle_path(this->config, record.source, record.name)) {
                record.bundleSource = true;
                record.bundlePath = bundlePath->string();
            }
        }

        if (registry_record_can_materialize_plugin(record) && !registry_record_passes_thin_layer_trust(this->config, record)) {
            record.script.clear();
            record.bootstrapScript.clear();
            record.bundleSource = false;
            record.bundlePath.clear();
        }

        const bool needsPayloadRefresh = fetchPayloads && registry_record_can_materialize_plugin(record) &&
            (record.script.empty() || !registry_record_matches_expected_hashes(record));
        if (needsPayloadRefresh) {
            if (const auto fetchedPayload = fetch_plugin_payload(this->config, record.source, record.name)) {
                record.script = fetchedPayload->first;
                record.bootstrapScript = fetchedPayload->second;
                if (const auto bundlePath = resolve_bundle_path(this->config, record.source, record.name)) {
                    record.bundleSource = true;
                    record.bundlePath = bundlePath->string();
                } else {
                    record.bundleSource = false;
                    record.bundlePath.clear();
                }
            }
        }

        if (registry_record_can_materialize_plugin(record) && !record.script.empty() && !registry_record_matches_expected_hashes(record)) {
            record.script.clear();
            record.bootstrapScript.clear();
            record.bundleSource = false;
            record.bundlePath.clear();
        }

        desiredRecords[record.name] = std::move(record);
    }

    std::set<std::string> namesToDelete;
    const bool needExistingRecords = replaceMissing || !originPathsToDelete.empty();
    const std::vector<RegistryRecord> existingRecords = needExistingRecords ? this->load_all_records() : std::vector<RegistryRecord>{};
    if (replaceMissing) {
        for (const RegistryRecord& existing : existingRecords) {
            if (!desiredRecords.contains(existing.name)) {
                namesToDelete.insert(existing.name);
            }
        }
    }
    if (!originPathsToDelete.empty()) {
        const std::set<std::string> originPathSet(originPathsToDelete.begin(), originPathsToDelete.end());
        for (const RegistryRecord& existing : existingRecords) {
            if (originPathSet.contains(existing.originPath)) {
                namesToDelete.insert(existing.name);
            }
        }
    }

    bool hasChanges = !namesToDelete.empty() || !metaValues.empty();
    for (const auto& [name, record] : desiredRecords) {
        const std::optional<RegistryRecord> existing = this->load_record(name);
        if (!existing.has_value() || !registry_records_equal(existing.value(), record)) {
            hasChanges = true;
            break;
        }
    }
    if (!hasChanges) {
        return true;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    for (const std::string& name : namesToDelete) {
        if (!delete_record_from_transaction(transaction, this->dbi, name)) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    for (const auto& [_, record] : desiredRecords) {
        if (!put_record_into_transaction(transaction, this->dbi, record)) {
            mdb_txn_abort(transaction);
            return false;
        }
    }

    if (!put_meta_values_into_transaction(transaction, this->metaDbi, metaValues)) {
        mdb_txn_abort(transaction);
        return false;
    }

    return mdb_txn_commit(transaction) == MDB_SUCCESS;
}

bool RegistryDatabase::write_records(const RegistrySourceMap& sources) const {
    if (!this->initialized) {
        return false;
    }

    std::vector<RegistryRecord> recordsToWrite;
    recordsToWrite.reserve(sources.size());

    for (const auto& [name, entry] : sources) {
        RegistryRecord record;
        record.name = registry_database_to_lower_copy(name);
        record.source = entry.source;
        record.alias = entry.alias;
        record.description = entry.description;
        record.role = entry.role;
        record.targetSystem = entry.targetSystem;
        record.capabilities = entry.capabilities;
        record.ecosystemScopes = entry.ecosystemScopes;
        record.writeScopes = entry.writeScopes;
        record.networkScopes = entry.networkScopes;
        record.privilegeLevel = entry.privilegeLevel;
        record.scriptSha256 = entry.scriptSha256;
        record.bootstrapSha256 = entry.bootstrapSha256;
        recordsToWrite.push_back(std::move(record));
    }

    return this->sync_records(recordsToWrite, true, false);
}
