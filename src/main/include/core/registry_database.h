#pragma once

#include "core/configuration.h"

#include <lmdb.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct RegistryRecord {
    std::string name;
    std::string source;
    bool alias{false};
    std::string description;
    std::string role;
    std::vector<std::string> capabilities;
    std::vector<std::string> ecosystemScopes;
    std::vector<RegistryWriteScope> writeScopes;
    std::vector<RegistryNetworkScope> networkScopes;
    std::string privilegeLevel;
    std::string scriptSha256;
    std::string bootstrapSha256;
    std::string script;
    std::string bootstrapScript;
    std::string bundlePath;
    bool bundleSource{false};
};

class RegistryDatabase {
    ReqPackConfig config;
    mutable std::mutex mutex;
    mutable MDB_env* env{nullptr};
    mutable MDB_dbi dbi{0};
    mutable bool initialized{false};
    mutable bool bootstrapped{false};

public:
    RegistryDatabase(const ReqPackConfig& config = default_reqpack_config());
    ~RegistryDatabase();

    RegistryDatabase(const RegistryDatabase&) = delete;
    RegistryDatabase& operator=(const RegistryDatabase&) = delete;

    bool ensureReady() const;
    std::optional<RegistryRecord> getRecord(const std::string& name) const;
    std::optional<RegistryRecord> resolveRecord(const std::string& name) const;
    std::optional<RegistryRecord> refreshRecord(const std::string& name, bool preferLatestTag = false) const;
    std::vector<RegistryRecord> getAllRecords() const;
    bool cacheScript(const std::string& name, const std::string& script) const;

private:
    bool initStorage() const;
    bool bootstrap_registry() const;
    bool write_records(const RegistrySourceMap& sources) const;
    std::optional<RegistryRecord> load_record(const std::string& name) const;
    bool put_record(const RegistryRecord& record) const;
};
