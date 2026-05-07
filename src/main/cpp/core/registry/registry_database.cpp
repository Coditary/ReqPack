#include "core/registry/registry_database.h"
#include "core/registry/registry_database_core.h"
#include "core/registry/registry_json_parser.h"
#include "core/common/version_compare.h"

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
#include <set>
#include <spawn.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

constexpr std::string_view META_SEPARATOR = "\n---\n";
constexpr unsigned int LMDB_MAX_DATABASES = 4;
constexpr std::size_t LMDB_MAP_SIZE = 32 * 1024 * 1024;
constexpr const char* REGISTRY_META_DATABASE_NAME = "meta";
constexpr const char* REGISTRY_META_KEY_REPO_URL = "repoUrl";
constexpr const char* REGISTRY_META_KEY_BRANCH = "branch";
constexpr const char* REGISTRY_META_KEY_PLUGINS_PATH = "remotePluginsPath";
constexpr const char* REGISTRY_META_KEY_SCHEMA_VERSION = "schemaVersion";
constexpr const char* REGISTRY_META_KEY_LAST_COMMIT = "lastCommit";

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

bool run_process_quiet(const std::vector<std::string>& arguments);
std::optional<std::string> run_process_capture_stdout(const std::vector<std::string>& arguments);
std::string trim_copy(const std::string& value);

bool is_json_registry_remote(const ReqPackConfig& config) {
    return registry_database_is_git_source(config.registry.remoteUrl) && !config.registry.remotePluginsPath.empty();
}

std::vector<std::filesystem::path> collect_registry_json_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return files;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            return {};
        }
        if (!it->is_regular_file() || it->path().extension() != ".json") {
            continue;
        }
        files.push_back(it->path());
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string path_to_generic_string(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool path_has_json_extension(const std::string& path) {
    return std::filesystem::path(path).extension() == ".json";
}

std::optional<std::string> git_repository_head_commit(const std::filesystem::path& repositoryPath) {
    const std::optional<std::string> output = run_process_capture_stdout({
        "git", "-C", repositoryPath.string(), "rev-parse", "--verify", "HEAD"
    });
    if (!output.has_value()) {
        return std::nullopt;
    }
    return trim_copy(output.value());
}

bool git_commit_exists(const std::filesystem::path& repositoryPath, const std::string& commit) {
    if (commit.empty()) {
        return false;
    }
    return run_process_quiet({
        "git", "-C", repositoryPath.string(), "rev-parse", "--verify", "--quiet", commit + "^{commit}"
    });
}

struct RegistryDiffEntry {
    char status{'?'};
    std::string path;
};

std::optional<std::vector<RegistryDiffEntry>> git_registry_diff(
    const std::filesystem::path& repositoryPath,
    const std::string& oldCommit,
    const std::string& newCommit,
    const std::string& pluginsPath
) {
    const std::optional<std::string> output = run_process_capture_stdout({
        "git",
        "-C",
        repositoryPath.string(),
        "diff",
        "--name-status",
        oldCommit + ".." + newCommit,
        "--",
        pluginsPath
    });
    if (!output.has_value()) {
        return std::nullopt;
    }

    std::vector<RegistryDiffEntry> entries;
    std::istringstream stream(output.value());
    std::string line;
    while (std::getline(stream, line)) {
        line = trim_copy(line);
        if (line.empty()) {
            continue;
        }

        std::istringstream lineStream(line);
        std::string status;
        std::string path;
        if (!(lineStream >> status)) {
            return std::nullopt;
        }
        if (status.empty()) {
            return std::nullopt;
        }

        if (status[0] == 'R' || status[0] == 'C') {
            std::string oldPath;
            std::string newPath;
            if (!(lineStream >> oldPath >> newPath)) {
                return std::nullopt;
            }
            path = newPath;
            entries.push_back({status[0], oldPath});
            entries.push_back({'A', newPath});
            continue;
        }

        if (!(lineStream >> path)) {
            return std::nullopt;
        }
        entries.push_back({status[0], path});
    }

    return entries;
}

std::vector<RegistryRecord> registry_records_for_origin(
    const std::vector<RegistryRecord>& records,
    const std::string& originPath
) {
    std::vector<RegistryRecord> matches;
    for (const RegistryRecord& record : records) {
        if (record.originPath == originPath) {
            matches.push_back(record);
        }
    }
    return matches;
}

bool registry_records_equal(const RegistryRecord& left, const RegistryRecord& right) {
    return left.name == right.name &&
           left.source == right.source &&
           left.alias == right.alias &&
           left.originPath == right.originPath &&
           left.description == right.description &&
           left.role == right.role &&
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

std::optional<std::pair<std::string, std::string>> fetch_plugin_payload(
    const ReqPackConfig& config,
    const std::string& source,
    const std::string& pluginName
);

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

std::optional<std::string> run_process_capture_stdout(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return std::nullopt;
    }

    int outputPipe[2];
    if (::pipe(outputPipe) != 0) {
        return std::nullopt;
    }

    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        return std::nullopt;
    }

    bool actionsReady =
        posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
        posix_spawn_file_actions_adddup2(&fileActions, outputPipe[1], STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_addopen(&fileActions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, outputPipe[0]) == 0 &&
        posix_spawn_file_actions_addclose(&fileActions, outputPipe[1]) == 0;
    if (!actionsReady) {
        posix_spawn_file_actions_destroy(&fileActions);
        (void)::close(outputPipe[0]);
        (void)::close(outputPipe[1]);
        return std::nullopt;
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
    (void)::close(outputPipe[1]);
    if (spawnResult != 0) {
        (void)::close(outputPipe[0]);
        return std::nullopt;
    }

    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t bytesRead = ::read(outputPipe[0], buffer, sizeof(buffer));
        if (bytesRead > 0) {
            output.append(buffer, static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
    (void)::close(outputPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            return std::nullopt;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }

    return output;
}

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> normalize_git_tag_for_compare(const std::string& tag) {
    const std::string trimmed = trim_copy(tag);
    if (trimmed.empty() || trimmed.ends_with("^{}")) {
        return std::nullopt;
    }

    std::string normalized = trimmed;
    if (!normalized.empty() && (normalized.front() == 'v' || normalized.front() == 'V') && normalized.size() > 1 &&
        std::isdigit(static_cast<unsigned char>(normalized[1])) != 0) {
        normalized.erase(normalized.begin());
    }

    bool hasDigit = false;
    for (unsigned char c : normalized) {
        if (std::isdigit(c) != 0) {
            hasDigit = true;
            continue;
        }
        if (std::isalpha(c) != 0 || c == '-' || c == '.' || c == '+') {
            continue;
        }
        return std::nullopt;
    }

    if (!hasDigit || normalized.empty()) {
        return std::nullopt;
    }

    return normalized;
}

std::optional<std::string> latest_git_tag_for_source(const std::string& source) {
    if (!registry_database_is_git_source(source)) {
        return std::nullopt;
    }

    const std::optional<std::string> output = run_process_capture_stdout({
        "git", "ls-remote", "--tags", "--refs", registry_database_git_source_url(source)
    });
    if (!output.has_value()) {
        return std::nullopt;
    }

    const std::vector<std::string> tags = registry_database_extract_git_tags(output.value());
    std::optional<std::string> bestTag;
    std::optional<std::string> bestNormalized;
    for (const std::string& tag : tags) {
        const std::optional<std::string> normalized = normalize_git_tag_for_compare(tag);
        if (!normalized.has_value()) {
            continue;
        }
        if (!bestNormalized.has_value() || version_compare_values(normalized.value(), bestNormalized.value(), VersionComparatorSpec{.profile = "semver"}) > 0) {
            bestTag = tag;
            bestNormalized = normalized;
        }
    }

    return bestTag;
}

bool sync_git_repository(const ReqPackConfig& config, const std::string& source, const std::string& pluginName) {
    const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(config, source, pluginName);
    const std::string repositoryUrl = registry_database_git_source_url(source);
    const std::string requestedRef = registry_database_git_source_ref(source);

    std::error_code directoryError;
    std::filesystem::create_directories(repositoryPath.parent_path(), directoryError);
    if (directoryError) {
        return false;
    }

    const std::filesystem::path gitDirectory = repositoryPath / ".git";
    if (std::filesystem::exists(gitDirectory)) {
        if (requestedRef.empty()) {
            if (run_process_quiet({"git", "-C", repositoryPath.string(), "pull", "--ff-only", "--quiet"})) {
                return true;
            }
        } else {
            const bool fetched = run_process_quiet({"git", "-C", repositoryPath.string(), "fetch", "--tags", "--quiet", "origin"});
            const bool checkedOut = fetched &&
                (run_process_quiet({"git", "-C", repositoryPath.string(), "checkout", "--quiet", requestedRef}) ||
                 run_process_quiet({"git", "-C", repositoryPath.string(), "checkout", "--quiet", "origin/" + requestedRef}));
            if (checkedOut) {
                if (run_process_quiet({"git", "-C", repositoryPath.string(), "rev-parse", "--verify", "--quiet", "origin/" + requestedRef})) {
                    (void)run_process_quiet({"git", "-C", repositoryPath.string(), "reset", "--hard", "origin/" + requestedRef});
                }
                return true;
            }
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
        "--quiet",
        repositoryUrl,
        repositoryPath.string()
    }) && (requestedRef.empty() || (
        run_process_quiet({"git", "-C", repositoryPath.string(), "checkout", "--quiet", requestedRef}) ||
        run_process_quiet({"git", "-C", repositoryPath.string(), "checkout", "--quiet", "origin/" + requestedRef})
    ));
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

std::optional<RegistryRecord> refreshed_record_payload(
    const ReqPackConfig& config,
    RegistryRecord record,
    bool preferLatestTag
) {
    if (record.alias) {
        return record;
    }

    if (!registry_record_passes_thin_layer_trust(config, record)) {
        return std::nullopt;
    }

    std::string sourceForPayload = record.source;
    if (preferLatestTag && registry_database_is_git_source(record.source)) {
        if (const std::optional<std::string> latestTag = latest_git_tag_for_source(record.source)) {
            sourceForPayload = registry_database_git_source_with_ref(record.source, latestTag.value());
        }
    }

    if (const auto fetchedPayload = fetch_plugin_payload(config, sourceForPayload, record.name)) {
        record.script = fetchedPayload->first;
        record.bootstrapScript = fetchedPayload->second;
        if (const auto bundlePath = resolve_bundle_path(config, sourceForPayload, record.name)) {
            record.bundleSource = true;
            record.bundlePath = bundlePath->string();
        } else {
            record.bundleSource = false;
            record.bundlePath.clear();
        }
        if (!registry_record_matches_expected_hashes(record)) {
            return std::nullopt;
        }
        return record;
    }

    return std::nullopt;
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

}  // namespace

RegistryDatabase::RegistryDatabase(const ReqPackConfig& config) : config(config) {}

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

        if (!record.alias && !record.bundleSource) {
            if (const auto bundlePath = resolve_bundle_path(this->config, record.source, record.name)) {
                record.bundleSource = true;
                record.bundlePath = bundlePath->string();
            }
        }

        if (!record.alias && !registry_record_passes_thin_layer_trust(this->config, record)) {
            record.script.clear();
            record.bootstrapScript.clear();
            record.bundleSource = false;
            record.bundlePath.clear();
        }

        const bool needsPayloadRefresh = fetchPayloads && !record.alias &&
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

        if (!record.alias && !record.script.empty() && !registry_record_matches_expected_hashes(record)) {
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

bool RegistryDatabase::bootstrap_registry() const {
    const RegistrySourceMap explicitSources = collect_explicit_registry_sources(this->config);
    const auto seedExplicitSources = [&]() {
        return explicitSources.empty() || this->write_records(explicitSources);
    };

    if (!is_json_registry_remote(this->config)) {
        return seedExplicitSources();
    }

    const bool mainRegistryReady = [&]() {
        const std::string source = registry_database_git_source_with_ref(
            this->config.registry.remoteUrl,
            this->config.registry.remoteBranch
        );
        if (!sync_git_repository(this->config, source, "main-registry")) {
            return !this->load_all_records().empty();
        }

        const std::filesystem::path repositoryPath = registry_database_git_repository_cache_path(this->config, source, "main-registry");
        const std::filesystem::path pluginsRoot = repositoryPath / this->config.registry.remotePluginsPath;
        if (!std::filesystem::exists(pluginsRoot)) {
            return !this->load_all_records().empty();
        }

        const std::optional<std::string> currentCommit = git_repository_head_commit(repositoryPath);
        if (!currentCommit.has_value() || currentCommit->empty()) {
            return !this->load_all_records().empty();
        }

        const std::map<std::string, std::string> baseMetaValues = {
            {REGISTRY_META_KEY_REPO_URL, this->config.registry.remoteUrl},
            {REGISTRY_META_KEY_BRANCH, this->config.registry.remoteBranch},
            {REGISTRY_META_KEY_PLUGINS_PATH, this->config.registry.remotePluginsPath},
            {REGISTRY_META_KEY_SCHEMA_VERSION, "1"},
            {REGISTRY_META_KEY_LAST_COMMIT, *currentCommit},
        };

        const std::optional<std::string> previousCommit = this->load_meta_value(REGISTRY_META_KEY_LAST_COMMIT);
        if (!previousCommit.has_value() || previousCommit->empty() || !git_commit_exists(repositoryPath, *previousCommit)) {
            std::vector<RegistryRecord> records;
            try {
                for (const std::filesystem::path& file : collect_registry_json_files(pluginsRoot)) {
                    const RegistryJsonParseResult parsed = parse_registry_json_file(file);
                    records.insert(records.end(), parsed.records.begin(), parsed.records.end());
                }
            } catch (const std::exception&) {
                return !this->load_all_records().empty();
            }

            const bool synced = this->sync_records(records, false, true, baseMetaValues);
            return synced || !this->load_all_records().empty();
        }

        if (*previousCommit == *currentCommit) {
            return this->sync_records({}, false, false, baseMetaValues);
        }

        const std::optional<std::vector<RegistryDiffEntry>> diffEntries = git_registry_diff(
            repositoryPath,
            *previousCommit,
            *currentCommit,
            this->config.registry.remotePluginsPath
        );
        if (!diffEntries.has_value()) {
            std::vector<RegistryRecord> records;
            try {
                for (const std::filesystem::path& file : collect_registry_json_files(pluginsRoot)) {
                    const RegistryJsonParseResult parsed = parse_registry_json_file(file);
                    records.insert(records.end(), parsed.records.begin(), parsed.records.end());
                }
            } catch (const std::exception&) {
                return !this->load_all_records().empty();
            }

            const bool synced = this->sync_records(records, false, true, baseMetaValues);
            return synced || !this->load_all_records().empty();
        }

        std::set<std::string> originPathsToDelete;
        std::set<std::string> parsePaths;
        for (const RegistryDiffEntry& entry : *diffEntries) {
            if (!path_has_json_extension(entry.path)) {
                continue;
            }

            const std::filesystem::path absolutePath = repositoryPath / entry.path;
            if (entry.status == 'D' || entry.status == 'R') {
                originPathsToDelete.insert(path_to_generic_string(absolutePath));
                continue;
            }

            if (!std::filesystem::exists(absolutePath)) {
                originPathsToDelete.insert(path_to_generic_string(absolutePath));
                continue;
            }

            originPathsToDelete.insert(path_to_generic_string(absolutePath));
            parsePaths.insert(path_to_generic_string(absolutePath));
        }

        std::vector<RegistryRecord> records;
        try {
            for (const std::string& path : parsePaths) {
                const RegistryJsonParseResult parsed = parse_registry_json_file(path);
                records.insert(records.end(), parsed.records.begin(), parsed.records.end());
            }
        } catch (const std::exception&) {
            return !this->load_all_records().empty();
        }

        const bool synced = this->sync_records(
            records,
            false,
            false,
            baseMetaValues,
            std::vector<std::string>(originPathsToDelete.begin(), originPathsToDelete.end())
        );
        return synced || !this->load_all_records().empty();
    }();

    const bool explicitReady = seedExplicitSources();
    return explicitReady && (mainRegistryReady || !explicitSources.empty());
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
