#include <chrono>
#include <filesystem>
#include <fstream>
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

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

ReqPackConfig make_executor_test_config(const std::filesystem::path& root) {
    ReqPackConfig config;
    config.registry.pluginDirectory = (root / "plugins").string();
    config.registry.databasePath = (root / "registry-db").string();
    config.registry.autoLoadPlugins = true;
    config.registry.shutDownPluginsOnExit = true;
    config.execution.transactionDatabasePath = (root / "transactions").string();
    config.execution.checkVirtualFileSystemWrite = false;
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
      description = context.plugin.dir,
      homepage = context.plugin.script,
    }
  }
end
function plugin.search(context, prompt)
  return {
    {
      name = prompt,
      version = join(context.flags),
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

function plugin.getName() return "txn-plugin" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "txn" } end
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
  if packages[1] ~= nil then
    context.exec.run("printf '%s' '" .. packages[1].name .. "' > '" .. context.plugin.dir .. "/last-install.txt'")
  end
  return packages[1] == nil or packages[1].name ~= "fail"
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
    CHECK(packages[0].description == (tempDir.path() / "plugins" / "query").string());
    CHECK(packages[0].homepage == scriptPath.string());
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
    CHECK(packages[0].description == scriptPath.string());
    CHECK(packages[0].author == "query");
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
