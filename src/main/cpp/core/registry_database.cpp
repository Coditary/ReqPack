#include "core/registry_database.h"
#include "core/registry_database_core.h"

#include <curl/curl.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr std::string_view META_SEPARATOR = "\n---\n";
constexpr unsigned int LMDB_MAX_DATABASES = 1;
constexpr std::size_t LMDB_MAP_SIZE = 32 * 1024 * 1024;

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::optional<std::pair<std::string, std::string>> read_plugin_payload_files(
    const std::filesystem::path& scriptPath,
    const std::filesystem::path& bootstrapPath
) {
    if (!std::filesystem::exists(scriptPath)) {
        return std::nullopt;
    }

    const std::string script = read_text_file(scriptPath);
    if (!registry_database_is_valid_plugin_script(script)) {
        return std::nullopt;
    }

    return std::make_pair(
        script,
        std::filesystem::exists(bootstrapPath) ? read_text_file(bootstrapPath) : std::string{}
    );
}

std::optional<std::pair<std::string, std::string>> read_plugin_directory(const std::filesystem::path& directory, const std::string& pluginName) {
    return read_plugin_payload_files(directory / (pluginName + ".lua"), directory / "bootstrap.lua");
}

std::optional<std::filesystem::path> plugin_bundle_root(const std::filesystem::path& basePath, const std::string& pluginName) {
    const std::vector<std::filesystem::path> candidates = {
        basePath / "plugins" / pluginName,
        basePath / pluginName,
        basePath
    };

    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate / (pluginName + ".lua"))) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::uint64_t fnv1a_hash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool run_process_quiet(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return false;
    }

    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        return false;
    }

    const bool actionsReady =
        posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_addopen(&fileActions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_addopen(&fileActions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
    if (!actionsReady) {
        posix_spawn_file_actions_destroy(&fileActions);
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(&pid, arguments.front().c_str(), &fileActions, nullptr, argv.data(), ::environ);
    posix_spawn_file_actions_destroy(&fileActions);
    if (spawnResult != 0) {
        return false;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool sync_git_repository(const ReqPackConfig& config, const std::string& source, const std::string& pluginName) {
    const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(config, source, pluginName);

    std::error_code directoryError;
    std::filesystem::create_directories(repositoryPath.parent_path(), directoryError);
    if (directoryError) {
        return false;
    }

    const std::filesystem::path gitDirectory = repositoryPath / ".git";
    if (std::filesystem::exists(gitDirectory)) {
        if (run_process_quiet({"git", "-C", repositoryPath.string(), "pull", "--ff-only", "--quiet"})) {
            return true;
        }

        std::error_code removeError;
        std::filesystem::remove_all(repositoryPath, removeError);
        if (removeError) {
            return false;
        }
    } else if (std::filesystem::exists(repositoryPath)) {
        std::error_code removeError;
        std::filesystem::remove_all(repositoryPath, removeError);
        if (removeError) {
            return false;
        }
    }

    return run_process_quiet({
        "git",
        "clone",
        "--depth",
        "1",
        "--single-branch",
        "--quiet",
        registry_database_git_source_url(source),
        repositoryPath.string()
    });
}

std::optional<std::pair<std::string, std::string>> read_plugin_repository(const std::filesystem::path& repositoryPath, const std::string& pluginName) {
    if (const auto bundleRoot = plugin_bundle_root(repositoryPath, pluginName)) {
        return read_plugin_directory(bundleRoot.value(), pluginName);
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> resolve_bundle_path(const ReqPackConfig& config, const std::string& source, const std::string& pluginName) {
    const std::filesystem::path sourcePath(source);
    if (std::filesystem::exists(sourcePath) && std::filesystem::is_directory(sourcePath)) {
        if (const auto bundleRoot = plugin_bundle_root(sourcePath, pluginName)) {
            return bundleRoot;
        }

        if (std::filesystem::exists(sourcePath / (pluginName + ".lua"))) {
            return sourcePath;
        }
    }

    if (registry_database_is_git_source(source)) {
        const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(config, source, pluginName);
        if (const auto bundleRoot = plugin_bundle_root(repositoryPath, pluginName)) {
            return bundleRoot;
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> fetch_git_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
) {
    if (!sync_git_repository(config, source, pluginName)) {
        return std::nullopt;
    }

    return read_plugin_repository(registry_database_git_repository_cache_path(config, source, pluginName), pluginName);
}

std::size_t write_to_string(void* contents, std::size_t size, std::size_t nmemb, void* userp) {
    const std::size_t bytes = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<const char*>(contents), bytes);
    return bytes;
}

std::optional<std::string> fetch_text(const ReqPackConfig& config, const std::string& source) {
    if (source.empty()) {
        return std::nullopt;
    }

    if (source.find("://") == std::string::npos) {
        const std::string content = read_text_file(source);
        return content.empty() ? std::nullopt : std::optional<std::string>(content);
    }

    std::string buffer;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return std::nullopt;
    }

    curl_easy_setopt(curl, CURLOPT_URL, source.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, config.downloader.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, config.downloader.connectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config.downloader.requestTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config.downloader.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    long statusCode = 0;
    const CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || statusCode >= 400 || buffer.empty()) {
        return std::nullopt;
    }

    return buffer;
}

std::optional<std::pair<std::string, std::string>> fetch_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
) {
    if (source.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path sourcePath(source);
    if (std::filesystem::exists(sourcePath) && std::filesystem::is_directory(sourcePath)) {
        return read_plugin_directory(sourcePath, pluginName);
    }

    if (registry_database_is_git_source(source)) {
        return fetch_git_plugin_payload(config, source, pluginName);
    }

    if (source.find("://") == std::string::npos) {
        const std::string script = read_text_file(sourcePath);
        if (!registry_database_is_valid_plugin_script(script)) {
            return std::nullopt;
        }

        const std::filesystem::path bootstrapPath = sourcePath.parent_path() / "bootstrap.lua";
        return std::make_pair(
            script,
            std::filesystem::exists(bootstrapPath) ? read_text_file(bootstrapPath) : std::string{}
        );
    }

    const std::optional<std::string> script = fetch_text(config, source);
    if (!script.has_value() || !registry_database_is_valid_plugin_script(script.value())) {
        return std::nullopt;
    }

    return std::make_pair(script.value(), std::string{});
}

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

}  // namespace

RegistryDatabase::RegistryDatabase(const ReqPackConfig& config) : config(config) {}

RegistryDatabase::~RegistryDatabase() {
    std::lock_guard<std::mutex> lock(this->mutex);
    if (this->initialized && this->env != nullptr) {
        mdb_dbi_close(this->env, this->dbi);
        mdb_env_close(this->env);
        this->env = nullptr;
        this->initialized = false;
        this->bootstrapped = false;
    }
}

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

    if (mdb_txn_commit(transaction) != MDB_SUCCESS) {
        mdb_dbi_close(this->env, this->dbi);
        mdb_env_close(this->env);
        this->env = nullptr;
        return false;
    }

    this->initialized = true;
    return true;
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

std::vector<RegistryRecord> RegistryDatabase::getAllRecords() const {
    std::vector<RegistryRecord> records;
    if (!this->ensureReady()) {
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
    return this->put_record(record.value());
}

bool RegistryDatabase::bootstrap_registry() const {
    const std::filesystem::path registrySourcePath = registry_source_file_path(this->config.registry.databasePath);
    if (!std::filesystem::exists(registrySourcePath) && !this->config.registry.remoteUrl.empty()) {
        const std::optional<std::string> remoteRegistry = fetch_text(this->config, this->config.registry.remoteUrl);
        if (remoteRegistry.has_value()) {
            std::filesystem::create_directories(registrySourcePath.parent_path());
            std::ofstream stream(registrySourcePath, std::ios::binary | std::ios::trunc);
            if (stream) {
                stream << *remoteRegistry;
            }
        }
    }

    return this->write_records(collect_registry_sources(this->config));
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
        record.bundleSource = false;
        record.bundlePath.clear();

        bool needsWrite = true;
        bool needsPayloadRefresh = !record.alias;
        if (const std::optional<RegistryRecord> existing = this->load_record(record.name)) {
            const bool sourceChanged = existing->source != record.source || existing->alias != record.alias;
            if (!sourceChanged) {
                record.script = existing->script;
                record.bootstrapScript = existing->bootstrapScript;
                record.bundleSource = existing->bundleSource;
                record.bundlePath = existing->bundlePath;
            }
            if (record.description.empty()) {
                record.description = existing->description;
            }

            const bool descriptionChanged = existing->description != record.description;

            if (!record.alias && !record.bundleSource) {
                if (const auto bundlePath = resolve_bundle_path(this->config, record.source, record.name)) {
                    record.bundleSource = true;
                    record.bundlePath = bundlePath->string();
                }
            }

            needsPayloadRefresh = !record.alias && (sourceChanged || record.script.empty());
            needsWrite = sourceChanged || descriptionChanged || needsPayloadRefresh ||
                         record.bundleSource != existing->bundleSource || record.bundlePath != existing->bundlePath;

            if (!needsWrite) {
                continue;
            }
        }

        if (!record.alias && needsPayloadRefresh) {
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
                needsWrite = true;
            }
        }

        if (needsWrite) {
            recordsToWrite.push_back(std::move(record));
        }
    }

    if (recordsToWrite.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(this->mutex);
    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(this->env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        return false;
    }

    for (const RegistryRecord& record : recordsToWrite) {
        if (!put_record_into_transaction(transaction, this->dbi, record)) {
            mdb_txn_abort(transaction);
            return false;
        }
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
