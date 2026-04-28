#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include <catch2/catch.hpp>

#include "test_helpers.h"

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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path add_plugin_script(const std::filesystem::path& pluginRoot, const std::string& pluginName, const std::string& content) {
    const std::filesystem::path scriptPath = pluginRoot / pluginName / (pluginName + ".lua");
    write_file(scriptPath, content);
    return scriptPath;
}

std::filesystem::path write_config(const std::filesystem::path& root, const std::filesystem::path& pluginDirectory) {
    const std::filesystem::path configPath = root / "config.lua";
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (root / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (root / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "}\n");
    return configPath;
}

std::string run_reqpack(const std::filesystem::path& workspace, const std::filesystem::path& configPath, const std::vector<std::string>& arguments) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && " + escape_shell_arg((build_root() / "ReqPack").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

const char* ORCHESTRATOR_PLUGIN = R"(
plugin = {}

function copy_packages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getSecurityMetadata()
  return {
    osvEcosystem = "demo-osv",
    purlType = "generic",
    versionComparatorProfile = "lexicographic",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return copy_packages(packages) end
function plugin.install(context, packages)
  local path = context.plugin.dir .. "/state/install.txt"
  context.exec.run("mkdir -p '" .. context.plugin.dir .. "/state' && printf '%s' '" .. packages[1].name .. "' > '" .. path .. "'")
  return true
end
function plugin.installLocal(context, path)
  local target = context.plugin.dir .. "/state/local.txt"
  context.exec.run("mkdir -p '" .. context.plugin.dir .. "/state' && printf '%s' '" .. path .. "' > '" .. target .. "'")
  return true
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context)
  return {
    {
      name = context.plugin.id,
      version = "1.0.0",
      description = "listed from " .. context.plugin.dir,
    }
  }
end
function plugin.search(context, prompt)
  return {
    {
      name = prompt,
      version = "2.0.0",
      description = "searched by " .. context.plugin.id,
    }
  }
end
function plugin.info(context, package)
  return {
    name = package,
    version = "3.0.0",
    description = "info from " .. context.plugin.id,
  }
end
function plugin.shutdown() return true end
)";

}  // namespace

TEST_CASE("orchestrator list command loads plugin from workspace plugins directory", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-workspace-list"};
    const std::filesystem::path configuredPluginDirectory = tempDir.path() / "configured-plugins";
    const std::filesystem::path workspacePluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), configuredPluginDirectory);

    add_plugin_script(workspacePluginDirectory, "workspace", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"list", "workspace"});

    CHECK(output.find("[workspace] (list) workspace 1.0.0 - listed from " + (workspacePluginDirectory / "workspace").string()) != std::string::npos);
}

TEST_CASE("orchestrator search command prints executor search results", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-search"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "query", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"search", "query", "alpha", "beta"});

    CHECK(output.find("[query] (search) alpha beta 2.0.0 - searched by query") != std::string::npos);
}

TEST_CASE("orchestrator info command prints executor info result", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-info"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "query", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"info", "query", "sample", "ignored"});

    CHECK(output.find("[query] (info) sample 3.0.0 - info from query") != std::string::npos);
}

TEST_CASE("orchestrator install command plans validates and executes plugin install", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "apply", "sample"});
    (void)output;

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(installMarker));
    CHECK(read_file(installMarker) == "sample");
}

TEST_CASE("orchestrator sbom command exports planned graph without executing plugin install", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "sbom",
        "apply",
        "sample",
        "--output",
        outputPath.string(),
    });

    CHECK(output.find(outputPath.string()) != std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string sbom = read_file(outputPath);
    CHECK(sbom.find("\"bomFormat\": \"CycloneDX\"") != std::string::npos);
    CHECK(sbom.find("\"name\": \"sample\"") != std::string::npos);

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    CHECK_FALSE(std::filesystem::exists(installMarker));
}
