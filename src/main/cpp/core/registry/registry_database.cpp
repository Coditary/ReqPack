#include "core/registry/registry_database.h"

#include "registry_database_internal.h"

RegistryDatabase::RegistryDatabase(const ReqPackConfig& config) : config(config) {}

bool RegistryDatabase::ensureReady() const {
    if (!this->initStorage()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(this->mutex);
        if (this->bootstrapped) {
            return true;
        }
    }

    const bool bootstrapped = this->bootstrap_registry();
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->bootstrapped = bootstrapped;
    }

    return bootstrapped;
}

bool RegistryDatabase::refreshMainRegistry(bool* changed) const {
    if (changed != nullptr) {
        *changed = false;
    }

    if (!this->initStorage()) {
        return false;
    }

    bool needsBootstrap = false;
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        needsBootstrap = !this->bootstrapped;
    }

    if (needsBootstrap) {
        const bool bootstrapped = this->bootstrap_registry();
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->bootstrapped = bootstrapped;
        }
        if (changed != nullptr) {
            *changed = bootstrapped && is_json_registry_remote(this->config);
        }
        return bootstrapped;
    }

    return this->sync_main_registry(changed);
}

std::optional<RegistryRecord> RegistryDatabase::getRecord(const std::string& name) const {
    if (!this->ensureReady()) {
        return std::nullopt;
    }

    return this->load_record(name);
}

std::optional<RegistryRecord> RegistryDatabase::resolveRecord(const std::string& name) const {
    std::optional<RegistryRecord> record = this->getRecord(name);
    if (!record.has_value()) {
        return std::nullopt;
    }

    std::size_t guard = 0;
    while (record->alias && !record->source.empty() && guard < 16) {
        record = this->getRecord(record->source);
        if (!record.has_value()) {
            return std::nullopt;
        }
        ++guard;
    }

    return record;
}

std::optional<RegistryRecord> RegistryDatabase::refreshRecord(const std::string& name, bool preferLatestTag) const {
    if (!this->ensureReady()) {
        return std::nullopt;
    }

    std::optional<RegistryRecord> record = this->getRecord(name);
    if (!record.has_value()) {
        return std::nullopt;
    }

    std::size_t guard = 0;
    while (record->alias && !record->source.empty() && guard < 16) {
        record = this->getRecord(record->source);
        if (!record.has_value()) {
            return std::nullopt;
        }
        ++guard;
    }

    const std::optional<RegistryRecord> refreshed = refreshed_record_payload(this->config, record.value(), preferLatestTag);
    if (!refreshed.has_value()) {
        return std::nullopt;
    }
    if (!this->put_record(refreshed.value())) {
        return std::nullopt;
    }
    return refreshed;
}

std::vector<RegistryRecord> RegistryDatabase::getAllRecords() const {
    if (!this->ensureReady()) {
        return {};
    }
    return this->load_all_records();
}

bool RegistryDatabase::cacheScript(const std::string& name, const std::string& script) const {
    if (!this->ensureReady()) {
        return false;
    }

    if (!registry_database_is_valid_plugin_script(script)) {
        return false;
    }

    std::optional<RegistryRecord> record = this->getRecord(name);
    if (!record.has_value()) {
        return false;
    }

    record->script = script;
    if (!registry_record_matches_expected_hashes(record.value())) {
        return false;
    }
    return this->put_record(record.value());
}
