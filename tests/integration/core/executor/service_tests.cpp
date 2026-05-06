#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <catch2/catch.hpp>

#include "core/executor.h"

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() /
            (prefix + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& target) : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(target);
    }

    ~ScopedCurrentPath() {
        std::error_code error;
        std::filesystem::current_path(original_, error);
    }

private:
    std::filesystem::path original_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

ReqPackConfig make_executor_test_config(const std::filesystem::path& root) {
    ReqPackConfig config;
    config.registry.pluginDirectory = (root / "plugins").string();
    config.registry.databasePath = (root / "registry-db").string();
    config.registry.autoLoadPlugins = true;
    config.registry.shutDownPluginsOnExit = true;
    config.execution.transactionDatabasePath = (root / "transactions").string();
    config.execution.checkVirtualFileSystemWrite = false;
    config.history.historyPath = (root / "history").string();
    return config;
}

std::filesystem::path add_plugin_script(const std::filesystem::path& pluginRoot, const std::string& pluginName, const std::string& content) {
    const std::filesystem::path scriptPath = pluginRoot / pluginName / (pluginName + ".lua");
    write_file(scriptPath, content);
    return scriptPath;
}

Graph make_linear_graph(const std::vector<Package>& packages) {
    Graph graph;
    std::vector<Graph::vertex_descriptor> vertices;
    vertices.reserve(packages.size());
    for (const Package& package : packages) {
        vertices.push_back(boost::add_vertex(package, graph));
    }
    for (std::size_t index = 1; index < vertices.size(); ++index) {
        boost::add_edge(vertices[index - 1], vertices[index], graph);
    }
    return graph;
}

const TransactionItemRecord* find_item(const std::vector<TransactionItemRecord>& items, const std::string& name) {
    for (const TransactionItemRecord& item : items) {
        if (item.package.name == name) {
            return &item;
        }
    }
    return nullptr;
}

const InstalledEntry* find_installed(
    const std::vector<InstalledEntry>& entries,
    const std::string& system,
    const std::string& name,
    const std::string& version
) {
    for (const InstalledEntry& entry : entries) {
        if (entry.system == system && entry.name == name && entry.version == version) {
            return &entry;
        }
    }
    return nullptr;
}

const char* QUERY_PLUGIN = R"(
plugin = {}

local function join(values)
  if values == nil then
    return ""
  end
  return table.concat(values, "|")
end

function plugin.getName() return "query-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "query" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context)
  return {
    {
      name = context.plugin.id,
      version = join(context.flags),
      type = "doc",
      architecture = "noarch",
      description = context.plugin.dir,
      homepage = context.plugin.script,
    }
  }
end
function plugin.outdated(context)
  return {
    {
      name = context.plugin.id,
      version = "1.0.0",
      latestVersion = "2.0.0",
      type = "doc",
      architecture = "noarch",
      description = context.plugin.dir,
    }
  }
end
function plugin.search(context, prompt)
  return {
    {
      name = prompt,
      version = join(context.flags),
      type = "doc",
      architecture = "noarch",
      description = context.plugin.script,
      author = context.plugin.id,
    }
  }
end
function plugin.info(context, package)
  return {
    name = package,
    version = join(context.flags),
    description = context.plugin.bootstrap,
    homepage = context.plugin.dir,
    author = context.plugin.id,
    email = "query@example.test",
  }
end
function plugin.shutdown() return true end
)";

const char* TRANSACTION_PLUGIN = R"(
plugin = {}

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function load_installed(dir)
  local installed = {}
  local path = dir .. "/installed.txt"
  local result = reqpack.exec.run("test -f " .. shell_quote(path) .. " && cat " .. shell_quote(path))
  if result.success and result.stdout ~= nil then
    for line in string.gmatch(result.stdout, "[^\r\n]+") do
      if line ~= "" then installed[line] = true end
    end
  end
  return installed
end

local function save_installed(dir, installed)
  local names = {}
  for name, present in pairs(installed) do
    if present then
      names[#names + 1] = name
    end
  end
  table.sort(names)
  local content = table.concat(names, "\n")
  if #content > 0 then content = content .. "\n" end
  reqpack.exec.run("printf '%s' " .. shell_quote(content) .. " > " .. shell_quote(dir .. "/installed.txt"))
end

local function record_call(dir, names)
  local line = table.concat(names, ",")
  reqpack.exec.run("printf '%s\\n' " .. shell_quote(line) .. " >> " .. shell_quote(dir .. "/calls.txt"))
  reqpack.exec.run("printf '%s' " .. shell_quote(line) .. " > " .. shell_quote(dir .. "/last-install.txt"))
end

function plugin.getName() return "txn-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "txn" } end
function plugin.getMissingPackages(packages)
  local installed = load_installed(REQPACK_PLUGIN_DIR)
  local missing = {}
  for _, package in ipairs(packages) do
    if package.name ~= "already" and not installed[package.name] then
      missing[#missing + 1] = package
    end
  end
  return missing
end
function plugin.install(context, packages)
  local names = {}
  local installed = load_installed(context.plugin.dir)
  local should_fail = false
  for _, package in ipairs(packages) do
    names[#names + 1] = package.name
    if package.name == "fail" then
      should_fail = true
    else
      installed[package.name] = true
    end
  end
  if #names > 0 then
    record_call(context.plugin.dir, names)
    save_installed(context.plugin.dir, installed)
  end
  return not should_fail
end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* RECOVERY_PLUGIN = R"(
plugin = {}

function plugin.getName() return "recovery-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "recovery" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    if package.name ~= "already" then
      missing[#missing + 1] = package
    end
  end
  return missing
end
function plugin.install(context, packages)
  return context.flags ~= nil and context.flags[1] == "--resume" and packages[1] ~= nil and packages[1].name == "recover"
end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* LOCAL_PLUGIN = R"(
plugin = {}

function plugin.getName() return "local-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "local" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return false end
function plugin.installLocal(context, path)
  return context.flags ~= nil and context.flags[1] == "--local-ok" and string.match(path, "artifact%.rpm$") ~= nil
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* STOP_ON_FAILURE_PLUGIN = R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "group" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages)
  return REQPACK_PLUGIN_ID ~= "stopper"
end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* PARTIAL_BATCH_PLUGIN = R"(
plugin = { installed = {} }

function plugin.getName() return "partial-batch-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "batch" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    if not plugin.installed[package.name] then
      missing[#missing + 1] = package
    end
  end
  return missing
end
function plugin.install(context, packages)
  local should_fail = false
  for _, package in ipairs(packages) do
    if package.name == "missing" then
      context.events.unavailable(package.name)
      should_fail = true
    else
      plugin.installed[package.name] = true
    end
  end
  return not should_fail
end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* HISTORY_SYNC_PLUGIN = R"(
plugin = {}

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function installed_path(dir)
  return dir .. "/installed.txt"
end

local function load_installed(dir)
  local items = {}
  local path = installed_path(dir)
  local result = reqpack.exec.run("test -f " .. shell_quote(path) .. " && cat " .. shell_quote(path))
  if result.success and result.stdout ~= nil then
    for line in string.gmatch(result.stdout, "[^\r\n]+") do
      local name, version = line:match("^(.-)\t(.+)$")
      if name and version then
        items[#items + 1] = { name = name, version = version }
      end
    end
  end
  return items
end

local function save_installed(dir, items)
  local lines = {}
  for _, item in ipairs(items) do
    lines[#lines + 1] = item.name .. "\t" .. item.version
  end
  table.sort(lines)
  local content = table.concat(lines, "\n")
  if #content > 0 then
    content = content .. "\n"
  end
  reqpack.exec.run("mkdir -p " .. shell_quote(dir))
  reqpack.exec.run("printf '%s' " .. shell_quote(content) .. " > " .. shell_quote(installed_path(dir)))
end

local function resolved_version(package)
  if package.version ~= nil and package.version ~= "" then
    return package.version
  end
  if package.name == "actual" then
    return "1.2.3"
  end
  return "resolved-" .. package.name
end

function plugin.getName() return "history-sync-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "history" } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages)
  local installed = load_installed(context.plugin.dir)
  for _, package in ipairs(packages) do
    installed[#installed + 1] = { name = package.name, version = resolved_version(package) }
  end
  save_installed(context.plugin.dir, installed)
  return true
end
function plugin.installLocal(context, path) return false end
function plugin.remove(context, packages)
  local installed = load_installed(context.plugin.dir)
  local kept = {}
  for _, item in ipairs(installed) do
    local removed = false
    for _, package in ipairs(packages) do
      if item.name == package.name and (package.version == nil or package.version == "" or item.version == package.version) then
        removed = true
        break
      end
    end
    if not removed then
      kept[#kept + 1] = item
    end
  end
  save_installed(context.plugin.dir, kept)
  return true
end
function plugin.update(context, packages)
  return plugin.install(context, packages)
end
function plugin.list(context)
  local items = load_installed(context.plugin.dir)
  for _, item in ipairs(items) do
    item.description = "Installed from plugin state"
  end
  return items
end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  for _, item in ipairs(load_installed(context.plugin.dir)) do
    if item.name == package then
      return { name = item.name, version = item.version, description = "Installed from plugin state" }
    end
  end
  return { name = package, version = "unknown", description = "missing" }
end
function plugin.resolvePackage(context, package)
  local resolved = package
  resolved.version = resolved_version(package)
  return resolved
end
function plugin.shutdown() return true end
)";

const char* PROXY_PLUGIN = R"(
plugin = {}

function plugin.getName() return "proxy-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "proxy", "test" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.resolveProxyRequest(context, request)
  return {
    targetSystem = context.proxy.default,
    packages = request.packages,
    flags = { "--proxy-routed" },
  }
end
function plugin.shutdown() return true end
)";

const char* OWNER_CHAIN_PLUGIN = R"(
plugin = {}

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function installed_path(dir)
  return dir .. "/installed.txt"
end

local function load_installed(dir)
  local installed = {}
  local result = reqpack.exec.run("test -f " .. shell_quote(installed_path(dir)) .. " && cat " .. shell_quote(installed_path(dir)))
  if result.success and result.stdout ~= nil then
    for line in string.gmatch(result.stdout, "[^\r\n]+") do
      installed[line] = true
    end
  end
  return installed
end

local function save_installed(dir, installed)
  local names = {}
  for name, present in pairs(installed) do
    if present then
      names[#names + 1] = name
    end
  end
  table.sort(names)
  local content = table.concat(names, "\n")
  if #content > 0 then
    content = content .. "\n"
  end
  reqpack.exec.run("mkdir -p " .. shell_quote(dir))
  reqpack.exec.run("printf '%s' " .. shell_quote(content) .. " > " .. shell_quote(installed_path(dir)))
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getCategories() return { "pkg", "owner" } end
function plugin.getRequirements()
  if REQPACK_PLUGIN_ID == "app" or REQPACK_PLUGIN_ID == "other" then
    return { { system = "dep", name = "maven" } }
  end
  return {}
end
function plugin.getMissingPackages(packages)
  local installed = load_installed(REQPACK_PLUGIN_DIR)
  local missing = {}
  for _, package in ipairs(packages) do
    if not installed[package.name] then
      missing[#missing + 1] = package
    end
  end
  return missing
end
function plugin.install(context, packages)
  local installed = load_installed(context.plugin.dir)
  for _, package in ipairs(packages) do
    installed[package.name] = true
  end
  save_installed(context.plugin.dir, installed)
  return true
end
function plugin.installLocal(context, path) return false end
function plugin.remove(context, packages)
  local installed = load_installed(context.plugin.dir)
  for _, package in ipairs(packages) do
    installed[package.name] = nil
  end
  save_installed(context.plugin.dir, installed)
  return true
end
function plugin.update(context, packages) return plugin.install(context, packages) end
function plugin.list(context)
  local items = {}
  for name, _ in pairs(load_installed(context.plugin.dir)) do
    items[#items + 1] = { name = name, version = "1.0.0", description = REQPACK_PLUGIN_ID }
  end
  table.sort(items, function(left, right) return left.name < right.name end)
  return items
end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  if load_installed(REQPACK_PLUGIN_DIR)[package] then
    return { name = package, version = "1.0.0", description = REQPACK_PLUGIN_ID }
  end
  return { name = package, version = "unknown", description = "missing" }
end
function plugin.shutdown() return true end
)";

}  // namespace

TEST_CASE("executor list dispatches flags and plugin context", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-list"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());

    const std::filesystem::path scriptPath = add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::LIST;
    request.system = "query";
    request.flags = {"--installed", "--json"};

    CHECK_FALSE(registry.isLoaded("query"));

    const std::vector<PackageInfo> packages = executer.list(request);

    REQUIRE(packages.size() == 1);
    CHECK(registry.isLoaded("query"));
    CHECK(packages[0].name == "query");
    CHECK(packages[0].version == "--installed|--json");
    CHECK(packages[0].packageType == "doc");
    CHECK(packages[0].architecture == "noarch");
    CHECK(packages[0].description == (tempDir.path() / "plugins" / "query").string());
    CHECK(packages[0].homepage == scriptPath.string());
}

TEST_CASE("executor list and outdated apply arch and type post filters", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-list-outdated-filters"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request listRequest;
    listRequest.action = ActionType::LIST;
    listRequest.system = "query";
    listRequest.flags = {"arch=noarch", "type=doc"};

    const std::vector<PackageInfo> listed = executer.list(listRequest);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "query");

    listRequest.flags = {"arch=x86_64"};
    CHECK(executer.list(listRequest).empty());

    Request outdatedRequest;
    outdatedRequest.action = ActionType::OUTDATED;
    outdatedRequest.system = "query";
    outdatedRequest.flags = {"type=doc"};

    const std::vector<PackageInfo> outdated = executer.outdated(outdatedRequest);
    REQUIRE(outdated.size() == 1);
    CHECK(outdated[0].name == "query");
    CHECK(outdated[0].latestVersion == "2.0.0");

    outdatedRequest.flags = {"type=cli"};
    CHECK(executer.outdated(outdatedRequest).empty());
}

TEST_CASE("executor list rqp returns installed plugin wrappers and aliases", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-rqp-list"};
    ScopedCurrentPath scopedCurrentPath(tempDir.path());
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.registry.sources["query"] = RegistrySourceEntry{
        .source = (tempDir.path() / "sources" / "query.lua").string(),
        .description = "Query system",
        .role = "package-manager",
    };
    config.registry.sources["lookup"] = RegistrySourceEntry{
        .source = "query",
        .alias = true,
        .description = "Alias for query",
    };

    add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::LIST;
    request.system = "rqp";

    const std::vector<PackageInfo> packages = executer.list(request);

    REQUIRE(packages.size() == 3);

    const auto find_package = [&](const std::string& name) -> const PackageInfo* {
        for (const PackageInfo& package : packages) {
            if (package.name == name) {
                return &package;
            }
        }
        return nullptr;
    };

    const PackageInfo* builtin = find_package("rqp");
    REQUIRE(builtin != nullptr);
    CHECK(builtin->system == "rqp");
    CHECK(builtin->status == "installed");
    CHECK(builtin->installed == "true");
    CHECK(builtin->packageType == "builtin");
    CHECK_FALSE(builtin->version.empty());

    const PackageInfo* query = find_package("query");
    REQUIRE(query != nullptr);
    CHECK(query->system == "rqp");
    CHECK(query->version == "1.0.0");
    CHECK(query->packageType == "package-manager");
    CHECK(query->description == "Query system");
    CHECK(query->status == "installed");
    CHECK(query->installed == "true");

    const PackageInfo* alias = find_package("lookup");
    REQUIRE(alias != nullptr);
    CHECK(alias->system == "rqp");
    CHECK(alias->version == "1.0.0");
    CHECK(alias->packageType == "alias");
    CHECK(alias->description == "Alias for query");
}

TEST_CASE("executor search joins package prompt and resolves aliases", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-search"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.planner.systemAliases["lookup"] = "query";

    const std::filesystem::path scriptPath = add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::SEARCH;
    request.system = "LOOKUP";
    request.packages = {"alpha", "beta", "gamma"};
    request.flags = {"--exact"};

    const std::vector<PackageInfo> packages = executer.search(request);

    REQUIRE(packages.size() == 1);
    CHECK(registry.isLoaded("query"));
    CHECK(packages[0].name == "alpha beta gamma");
    CHECK(packages[0].version == "--exact");
    CHECK(packages[0].packageType == "doc");
    CHECK(packages[0].architecture == "noarch");
    CHECK(packages[0].description == scriptPath.string());
    CHECK(packages[0].author == "query");
}

TEST_CASE("executor search applies arch and type post filters", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-search-filters"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::SEARCH;
    request.system = "query";
    request.packages = {"alpha"};
    request.flags = {"arch=noarch", "type=doc"};

    const std::vector<PackageInfo> packages = executer.search(request);
    REQUIRE(packages.size() == 1);
    CHECK(packages[0].name == "alpha");

    request.flags = {"arch=x86_64"};
    CHECK(executer.search(request).empty());

    request.flags = {"type=cli"};
    CHECK(executer.search(request).empty());
}

TEST_CASE("executor info forwards first package and flags", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-info"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());

    add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::INFO;
    request.system = "query";
    request.packages = {"first", "ignored"};
    request.flags = {"--verbose", "--raw"};

    const PackageInfo info = executer.info(request);

    CHECK(info.name == "first");
    CHECK(info.version == "--verbose|--raw");
    CHECK(info.description == (tempDir.path() / "plugins" / "query" / "bootstrap.lua").string());
    CHECK(info.homepage == (tempDir.path() / "plugins" / "query").string());
    CHECK(info.author == "query");
    CHECK(info.email == "query@example.test");
}

TEST_CASE("executor resolves proxy plugin requests before query dispatch", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-proxy-query"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.planner.proxies["java"].defaultTarget = "query";
    config.planner.proxies["java"].targets = {"query"};

    const std::filesystem::path scriptPath = add_plugin_script(tempDir.path() / "plugins", "query", QUERY_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "java", PROXY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Request request;
    request.action = ActionType::SEARCH;
    request.system = "java";
    request.packages = {"alpha", "beta"};

    const std::vector<PackageInfo> packages = executer.search(request);

    REQUIRE(packages.size() == 1);
    CHECK(packages[0].name == "alpha beta");
    CHECK(packages[0].version == "--proxy-routed");
    CHECK(packages[0].description == scriptPath.string());
}

TEST_CASE("executor read operations return empty values when plugin is unavailable", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-missing"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());

    Registry registry(config);
    Executer executer(&registry, config);

    Request request;
    request.system = "missing";
    request.packages = {"ghost"};

    CHECK(executer.list(request).empty());
    CHECK(executer.search(request).empty());

    const PackageInfo info = executer.info(request);
    CHECK(info.name.empty());
    CHECK(info.version.empty());
    CHECK(info.description.empty());
}

TEST_CASE("executor records transactional package results and leaves failed run active", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-transaction"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;

    add_plugin_script(tempDir.path() / "plugins", "txn", TRANSACTION_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph graph = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "txn", .name = "already"},
        Package{.action = ActionType::INSTALL, .system = "txn", .name = "ok"},
        Package{.action = ActionType::INSTALL, .system = "txn", .name = "fail"},
    });

    executer.execute(&graph);

    CHECK(read_file(tempDir.path() / "plugins" / "txn" / "calls.txt") == "ok,fail\n");

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());

    const std::optional<TransactionRunRecord> activeRun = database.getActiveRun();
    REQUIRE(activeRun.has_value());
    CHECK(activeRun->state == "failed");

    const std::vector<TransactionItemRecord> items = database.getRunItems(activeRun->id);
    REQUIRE(items.size() == 2);
    CHECK(find_item(items, "already") == nullptr);

    const TransactionItemRecord* okItem = find_item(items, "ok");
    REQUIRE(okItem != nullptr);
    CHECK(okItem->status == "success");
    CHECK(okItem->errorMessage.empty());

    const TransactionItemRecord* failItem = find_item(items, "fail");
    REQUIRE(failItem != nullptr);
    CHECK(failItem->status == "failed");
    CHECK(failItem->errorMessage == "plugin action failed");
}

TEST_CASE("executor recovers active run, reapplies flags, and commits reconciled items", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-recovery"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;

    add_plugin_script(tempDir.path() / "plugins", "recovery", RECOVERY_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    TransactionDatabase seedDatabase(config);
    REQUIRE(seedDatabase.ensureReady());

    const Package alreadyPackage{.action = ActionType::INSTALL, .system = "recovery", .name = "already"};
    const Package recoverPackage{.action = ActionType::INSTALL, .system = "recovery", .name = "recover"};
    const std::string runId = seedDatabase.createRun({alreadyPackage, recoverPackage}, {"--resume"});
    REQUIRE_FALSE(runId.empty());

    Executer executer(&registry, config);
    Graph graph;
    executer.execute(&graph);

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());
    CHECK_FALSE(database.getActiveRun().has_value());

    const std::vector<TransactionItemRecord> items = database.getRunItems(runId);
    REQUIRE(items.size() == 2);

    const TransactionItemRecord* alreadyItem = find_item(items, "already");
    REQUIRE(alreadyItem != nullptr);
    CHECK(alreadyItem->status == "committed");
    CHECK(alreadyItem->errorMessage.empty());

    const TransactionItemRecord* recoverItem = find_item(items, "recover");
    REQUIRE(recoverItem != nullptr);
    CHECK(recoverItem->status == "committed");
    CHECK(recoverItem->errorMessage.empty());
}

TEST_CASE("executor continues with current graph after recovering stale active run", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-recovery-continue"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;

    add_plugin_script(tempDir.path() / "plugins", "recovery", RECOVERY_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "txn", TRANSACTION_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);

    TransactionDatabase seedDatabase(config);
    REQUIRE(seedDatabase.ensureReady());

    const Package alreadyPackage{.action = ActionType::INSTALL, .system = "recovery", .name = "already"};
    const Package recoverPackage{.action = ActionType::INSTALL, .system = "recovery", .name = "recover"};
    const std::string recoveredRunId = seedDatabase.createRun({alreadyPackage, recoverPackage}, {"--resume"});
    REQUIRE_FALSE(recoveredRunId.empty());

    Executer executer(&registry, config);
    Graph graph = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "txn", .name = "ok"},
    });
    executer.execute(&graph);

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());
    CHECK_FALSE(database.getActiveRun().has_value());

    const std::vector<TransactionItemRecord> recoveredItems = database.getRunItems(recoveredRunId);
    REQUIRE(recoveredItems.size() == 2);
    const TransactionItemRecord* recoveredItem = find_item(recoveredItems, "recover");
    REQUIRE(recoveredItem != nullptr);
    CHECK(recoveredItem->status == "committed");

    const std::filesystem::path markerPath = tempDir.path() / "plugins" / "txn" / "last-install.txt";
    REQUIRE(std::filesystem::exists(markerPath));

    std::ifstream marker(markerPath, std::ios::binary);
    REQUIRE(marker.is_open());
    std::string markerValue;
    marker >> markerValue;
    CHECK(markerValue == "ok");
}

TEST_CASE("executor dispatches local install target through installLocal", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-local"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;

    add_plugin_script(tempDir.path() / "plugins", "localer", LOCAL_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    const std::filesystem::path localArtifact = tempDir.path() / "artifacts" / "artifact.rpm";
    write_file(localArtifact, "rpm-bytes");

    Graph graph = make_linear_graph({
        Package{
            .action = ActionType::INSTALL,
            .system = "localer",
            .name = "artifact.rpm",
            .sourcePath = localArtifact.string(),
            .localTarget = true,
            .flags = {"--local-ok"},
        },
    });

    executer.execute(&graph);

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());
    CHECK_FALSE(database.getActiveRun().has_value());
}

TEST_CASE("executor stopOnFirstFailure prevents later task groups from running", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-stop"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;
    config.execution.stopOnFirstFailure = true;

    add_plugin_script(tempDir.path() / "plugins", "stopper", STOP_ON_FAILURE_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "follower", STOP_ON_FAILURE_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph graph;
    const Graph::vertex_descriptor stopperVertex = boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "stopper", .name = "first"}, graph);
    const Graph::vertex_descriptor followerVertex = boost::add_vertex(Package{.action = ActionType::INSTALL, .system = "follower", .name = "second"}, graph);
    boost::add_edge(stopperVertex, followerVertex, graph);

    executer.execute(&graph);

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());

    const std::optional<TransactionRunRecord> activeRun = database.getActiveRun();
    REQUIRE(activeRun.has_value());
    CHECK(activeRun->state == "failed");

    const std::vector<TransactionItemRecord> items = database.getRunItems(activeRun->id);
    REQUIRE(items.size() == 2);

    const TransactionItemRecord* firstItem = find_item(items, "first");
    REQUIRE(firstItem != nullptr);
    CHECK(firstItem->status == "failed");
    CHECK(firstItem->errorMessage == "plugin action failed");

    const TransactionItemRecord* secondItem = find_item(items, "second");
    REQUIRE(secondItem != nullptr);
    CHECK(secondItem->status == "planned");
    CHECK(secondItem->errorMessage.empty());
}

TEST_CASE("executor keeps partial batch success and marks unavailable packages precisely", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-partial-batch"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = true;
    config.execution.deleteCommittedTransactions = false;

    add_plugin_script(tempDir.path() / "plugins", "batcher", PARTIAL_BATCH_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph graph = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "batcher", .name = "ok"},
        Package{.action = ActionType::INSTALL, .system = "batcher", .name = "missing"},
    });

    executer.execute(&graph);

    TransactionDatabase database(config);
    REQUIRE(database.ensureReady());

    const std::optional<TransactionRunRecord> activeRun = database.getActiveRun();
    REQUIRE(activeRun.has_value());
    CHECK(activeRun->state == "failed");

    const std::vector<TransactionItemRecord> items = database.getRunItems(activeRun->id);
    REQUIRE(items.size() == 2);

    const TransactionItemRecord* okItem = find_item(items, "ok");
    REQUIRE(okItem != nullptr);
    CHECK(okItem->status == "success");
    CHECK(okItem->errorMessage.empty());

    const TransactionItemRecord* missingItem = find_item(items, "missing");
    REQUIRE(missingItem != nullptr);
    CHECK(missingItem->status == "failed");
    CHECK(missingItem->errorMessage == "package unavailable");
}

TEST_CASE("executor refreshes history snapshot from authoritative plugin list", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-history-sync"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = false;

    add_plugin_script(tempDir.path() / "plugins", "history", HISTORY_SYNC_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph graph = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "history", .name = "actual"},
        Package{.action = ActionType::INSTALL, .system = "history", .name = "tool", .version = "1.0.0"},
        Package{.action = ActionType::INSTALL, .system = "history", .name = "tool", .version = "2.0.0"},
    });

    executer.setRequestedItemCount(3, true);
    executer.execute(&graph);

    const std::vector<InstalledEntry> entries = HistoryManager(config).loadInstalledState();
    REQUIRE(entries.size() == 3);
    CHECK(find_installed(entries, "history", "actual", "1.2.3") != nullptr);
    CHECK(find_installed(entries, "history", "tool", "1.0.0") != nullptr);
    CHECK(find_installed(entries, "history", "tool", "2.0.0") != nullptr);
}

TEST_CASE("executor keeps shared dependency installed until last owner is removed", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-owner-shared"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = false;

    add_plugin_script(tempDir.path() / "plugins", "app", OWNER_CHAIN_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "other", OWNER_CHAIN_PLUGIN);
    add_plugin_script(tempDir.path() / "plugins", "dep", OWNER_CHAIN_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph installGraph = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "dep", .name = "maven"},
        Package{.action = ActionType::INSTALL, .system = "app", .name = "alpha", .directRequest = true},
        Package{.action = ActionType::INSTALL, .system = "other", .name = "beta", .directRequest = true},
    });
    executer.setRequestedItemCount(2, false);
    executer.execute(&installGraph);

    std::vector<InstalledEntry> entries = HistoryManager(config).loadInstalledState();
    const InstalledEntry* dep = find_installed(entries, "dep", "maven", "1.0.0");
    REQUIRE(dep != nullptr);
    CHECK(dep->installMethod == "dependency");
    CHECK(dep->owners == std::vector<std::string>{
        installed_package_owner_id("app", "alpha"),
        installed_package_owner_id("other", "beta"),
    });

    Graph removeOneGraph = make_linear_graph({
        Package{.action = ActionType::REMOVE, .system = "app", .name = "alpha", .directRequest = true},
    });
    executer.setRequestedItemCount(1, false);
    executer.execute(&removeOneGraph);

    entries = HistoryManager(config).loadInstalledState();
    dep = find_installed(entries, "dep", "maven", "1.0.0");
    REQUIRE(dep != nullptr);
    CHECK(dep->owners == std::vector<std::string>{installed_package_owner_id("other", "beta")});

    Graph removeLastGraph = make_linear_graph({
        Package{.action = ActionType::REMOVE, .system = "other", .name = "beta", .directRequest = true},
    });
    executer.setRequestedItemCount(1, false);
    executer.execute(&removeLastGraph);

    entries = HistoryManager(config).loadInstalledState();
    CHECK(find_installed(entries, "dep", "maven", "1.0.0") == nullptr);
}

TEST_CASE("executor records explicit owner for already installed direct request", "[integration][executor][service]") {
    TempDir tempDir{"reqpack-executor-owner-explicit"};
    ReqPackConfig config = make_executor_test_config(tempDir.path());
    config.execution.useTransactionDb = false;

    add_plugin_script(tempDir.path() / "plugins", "dep", OWNER_CHAIN_PLUGIN);

    Registry registry(config);
    registry.scanDirectory(config.registry.pluginDirectory);
    Executer executer(&registry, config);

    Graph firstInstall = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "dep", .name = "maven", .directRequest = true},
    });
    executer.setRequestedItemCount(1, false);
    executer.execute(&firstInstall);

    Graph secondInstall = make_linear_graph({
        Package{.action = ActionType::INSTALL, .system = "dep", .name = "maven", .directRequest = true},
    });
    executer.setRequestedItemCount(1, false);
    executer.execute(&secondInstall);

    const std::vector<InstalledEntry> entries = HistoryManager(config).loadInstalledState();
    const InstalledEntry* dep = find_installed(entries, "dep", "maven", "1.0.0");
    REQUIRE(dep != nullptr);
    CHECK(dep->installMethod == "explicit");
    CHECK(dep->owners == std::vector<std::string>{installed_root_owner_id("dep", "maven")});
}
