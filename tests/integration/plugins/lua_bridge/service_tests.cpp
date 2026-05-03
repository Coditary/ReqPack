#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <catch2/catch.hpp>

#include "plugins/lua_bridge.h"
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

class StdoutCapture {
public:
    StdoutCapture() {
        std::cout.flush();
        std::fflush(stdout);
        originalFd_ = ::dup(STDOUT_FILENO);
        if (originalFd_ == -1) {
            throw std::runtime_error("failed to duplicate stdout");
        }

        file_ = std::tmpfile();
        if (file_ == nullptr) {
            ::close(originalFd_);
            originalFd_ = -1;
            throw std::runtime_error("failed to create stdout capture file");
        }

        if (::dup2(::fileno(file_), STDOUT_FILENO) == -1) {
            std::fclose(file_);
            file_ = nullptr;
            ::close(originalFd_);
            originalFd_ = -1;
            throw std::runtime_error("failed to redirect stdout");
        }
    }

    ~StdoutCapture() {
        restore();
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    std::string finish() {
        const std::string output = this->readAll();
        restore();
        return output;
    }

private:
    std::string readAll() {
        std::cout.flush();
        std::fflush(stdout);
        if (file_ == nullptr) {
            return {};
        }

        const long current = std::ftell(file_);
        std::rewind(file_);

        std::ostringstream buffer;
        char chunk[4096];
        while (std::fgets(chunk, static_cast<int>(sizeof(chunk)), file_) != nullptr) {
            buffer << chunk;
        }

        std::clearerr(file_);
        std::fseek(file_, current, SEEK_SET);
        return buffer.str();
    }
    void restore() {
        if (originalFd_ == -1) {
            return;
        }

        std::cout.flush();
        std::fflush(stdout);
        (void)::dup2(originalFd_, STDOUT_FILENO);
        ::close(originalFd_);
        originalFd_ = -1;
    }

    FILE* file_{nullptr};
    int originalFd_{-1};
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

PluginCallContext make_context(LuaBridge& bridge, const ReqPackConfig& config, std::vector<std::string> flags = {}) {
    return PluginCallContext{
        .pluginId = bridge.getPluginId(),
        .pluginDirectory = bridge.getPluginDirectory(),
        .scriptPath = bridge.getScriptPath(),
        .bootstrapPath = bridge.getBootstrapPath(),
        .flags = std::move(flags),
        .host = bridge.getRuntimeHost(),
        .repositories = repositories_for_ecosystem(config, bridge.getPluginId()),
    };
}

const char* BOOTSTRAP_SCRIPT = R"(
BOOTSTRAP_LABEL = "booted"
BOOTSTRAP_READY = "no"

function bootstrap()
  BOOTSTRAP_READY = "yes"
  return true
end
)";

const char* QUERY_PLUGIN = R"(
plugin = {}

function plugin.getName() return "query-bridge" end
function plugin.getVersion() return BOOTSTRAP_LABEL end
function plugin.getSecurityMetadata()
  return {
    osvEcosystem = "demo-osv",
    purlType = "generic",
    versionComparatorProfile = "lexicographic",
    versionTokenPattern = "[0-9]+|[A-Za-z]+",
  }
end
function plugin.getRequirements()
  return {
    {
      action = "install",
      system = "dnf",
      name = "curl",
      version = "8.0",
      sourcePath = "/tmp/curl.rpm",
      localTarget = true,
      flags = { "dep-flag" },
    }
  }
end
function plugin.getCategories() return { "query", BOOTSTRAP_READY } end
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = {
      action = package.action,
      system = package.system,
      name = package.name .. "-missing",
      version = package.version,
      sourcePath = package.sourcePath,
      localTarget = package.localTarget,
      flags = package.flags,
    }
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return path ~= "" end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context)
  return {
    {
      name = BOOTSTRAP_LABEL,
      version = context.flags[1],
      description = context.plugin.id,
    }
  }
end
function plugin.search(context, prompt)
  return {
    {
      name = prompt,
      version = BOOTSTRAP_READY,
      description = context.plugin.script,
    }
  }
end
function plugin.info(context, package)
  return {
    name = package,
    version = BOOTSTRAP_LABEL,
    description = context.plugin.bootstrap,
  }
end
function plugin.shutdown() return true end
)";

const char* CONTEXT_PLUGIN = R"(
plugin = {}

function copy_packages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end

function plugin.getName() return "bridge" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "bridge" } end
function plugin.getMissingPackages(packages) return copy_packages(packages) end
function plugin.install(context, packages)
  context.log.debug("debug-msg")
  context.log.info("info-msg")
  context.log.warn("warn-msg")
  context.log.error("error-msg")

  context.tx.status(17)
  context.tx.progress(33)
  context.tx.begin_step("phase one")
  context.tx.commit()
  context.tx.success()
  context.tx.failed("soft-fail")

  context.events.installed("demo-installed")
  context.events.listed("demo-listed")
  context.artifacts.register("artifact.json")

  local tmp = context.fs.get_tmp_dir()
  local tmp_check = context.exec.run("test -d '" .. tmp .. "'")
  local plain = context.exec.run("printf 'ctx-exec'")
  local ruled = context.exec.run("printf 'line:41\\n'", {
    initial = "scan",
    rules = {
      {
        state = "scan",
        source = "line",
        regex = "^line:(\\d+)$",
        actions = {
          { type = "progress", percent = "${1}" },
          { type = "event", name = "line_progress", payload = "${1}" },
        },
      },
    },
  })
  local global = reqpack.exec.run("printf 'global-exec'")

  local src = context.plugin.dir .. "/source.zip"
  local dst = context.plugin.dir .. "/downloaded.txt"
  local net_ok = context.net.download(src, dst) and context.exec.run("test -d '" .. dst .. "' && test -f '" .. dst .. "/source.txt'").success

  local meta_path = context.plugin.dir .. "/meta.txt"
  local meta_cmd = "printf '%s\\n%s\\n%s\\n%s\\n%s' '" .. context.flags[1] .. "' '" .. context.plugin.id .. "' '" .. plain.stdout .. "' '" .. global.stdout .. "' '" .. tmp .. "' > '" .. meta_path .. "'"
  local meta_write = context.exec.run(meta_cmd)

  return tmp_check.success and plain.success and plain.exitCode == 0 and plain.stdout == "ctx-exec" and ruled.success and global.success and global.stdout == "global-exec" and net_ok and meta_write.success and packages[1].name == "demo"
end
function plugin.installLocal(context, path) return path ~= "" end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* REPOSITORY_PLUGIN = R"(
plugin = {}

function plugin.getName() return "repo-bridge" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "repo" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  local repos = context.repositories
  local first = repos[1]
  local second = repos[2]
  local lines = {
    tostring(#repos),
    first.id,
    tostring(first.priority),
    first.auth.type,
    first.auth.token,
    first.validation.checksum,
    tostring(first.validation.tlsVerify),
    first.scope.include[1],
    tostring(first.snapshots),
    second.id,
    second.type,
    second.scope.exclude[1],
    second.tags[1],
  }
  return {
    name = package,
    version = table.concat(lines, "|"),
    description = repos[3] == nil and "only-current-ecosystem" or "unexpected-extra-repo",
  }
end
function plugin.shutdown() return true end
)";

}  // namespace

TEST_CASE("lua bridge loads bootstrap state and parses query values", "[integration][lua_bridge][service]") {
    TempDir tempDir{"reqpack-lua-bridge-query"};
    ReqPackConfig config;
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins" / "query";
    const std::filesystem::path scriptPath = pluginDirectory / "query.lua";

    write_file(pluginDirectory / "bootstrap.lua", BOOTSTRAP_SCRIPT);
    write_file(scriptPath, QUERY_PLUGIN);

    LuaBridge bridge(scriptPath.string(), config);
    CHECK(bridge.getName() == "query-bridge");
    CHECK(bridge.getVersion() == "booted");
    REQUIRE(bridge.getSecurityMetadata().has_value());
    CHECK(bridge.getSecurityMetadata()->osvEcosystem == "demo-osv");
    CHECK(bridge.getSecurityMetadata()->purlType == "generic");
    CHECK(bridge.getSecurityMetadata()->versionComparator.profile == "lexicographic");
    REQUIRE(bridge.init());

    const std::vector<std::string> categories = bridge.getCategories();
    REQUIRE(categories.size() == 2);
    CHECK(categories[0] == "query");
    CHECK(categories[1] == "yes");

    const std::vector<Package> requirements = bridge.getRequirements();
    REQUIRE(requirements.size() == 1);
    CHECK(requirements[0].action == ActionType::INSTALL);
    CHECK(requirements[0].system == "dnf");
    CHECK(requirements[0].name == "curl");
    CHECK(requirements[0].version == "8.0");
    CHECK(requirements[0].sourcePath == "/tmp/curl.rpm");
    CHECK(requirements[0].localTarget);
    CHECK(requirements[0].flags == std::vector<std::string>{"dep-flag"});

    const Package requested{
        .action = ActionType::INSTALL,
        .system = "query",
        .name = "demo",
        .version = "1.2.3",
        .sourcePath = "/tmp/demo.pkg",
        .localTarget = true,
        .flags = {"flag-a"},
    };
    const std::vector<Package> missing = bridge.getMissingPackages({requested});
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].action == ActionType::INSTALL);
    CHECK(missing[0].system == "query");
    CHECK(missing[0].name == "demo-missing");
    CHECK(missing[0].version == "1.2.3");
    CHECK(missing[0].sourcePath == "/tmp/demo.pkg");
    CHECK(missing[0].localTarget);
    CHECK(missing[0].flags == std::vector<std::string>{"flag-a"});

    const PluginCallContext context = make_context(bridge, config, {"--query-flag"});
    const std::vector<PackageInfo> listed = bridge.list(context);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "booted");
    CHECK(listed[0].version == "--query-flag");
    CHECK(listed[0].description == "query");

    const std::vector<PackageInfo> searched = bridge.search(context, "alpha beta");
    REQUIRE(searched.size() == 1);
    CHECK(searched[0].name == "alpha beta");
    CHECK(searched[0].version == "yes");
    CHECK(searched[0].description == scriptPath.string());

    const PackageInfo info = bridge.info(context, "artifact");
    CHECK(info.name == "artifact");
    CHECK(info.version == "booted");
    CHECK(info.description == (pluginDirectory / "bootstrap.lua").string());
}

TEST_CASE("lua bridge install exposes context namespaces and runtime host services", "[integration][lua_bridge][service]") {
    TempDir tempDir{"reqpack-lua-bridge-context"};
    ReqPackConfig config;
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins" / "bridge";
    const std::filesystem::path scriptPath = pluginDirectory / "bridge.lua";

    write_file(pluginDirectory / "source.txt", "download-payload\n");
    const std::string zipCommand = "zip -qj " + escape_shell_arg((pluginDirectory / "source.zip").string()) + " " + escape_shell_arg((pluginDirectory / "source.txt").string());
    REQUIRE(std::system(zipCommand.c_str()) == 0);
    write_file(scriptPath, CONTEXT_PLUGIN);

    Logger::instance().setLevel(spdlog::level::debug);
    LuaBridge bridge(scriptPath.string(), config);
    REQUIRE(bridge.init());

    StdoutCapture capture;
    const bool installed = bridge.install(
        make_context(bridge, config, {"--bridge-flag"}),
        {Package{.action = ActionType::INSTALL, .system = "bridge", .name = "demo"}}
    );
    Logger::instance().flush();
    const std::string output = capture.finish();

    REQUIRE(installed);

    const std::filesystem::path downloadedPath = pluginDirectory / "downloaded.txt";
    const std::filesystem::path metaPath = pluginDirectory / "meta.txt";
    REQUIRE(std::filesystem::exists(metaPath));
    CHECK(std::filesystem::is_directory(downloadedPath));
    CHECK(read_file(downloadedPath / "source.txt") == "download-payload\n");

    std::istringstream metaStream(read_file(metaPath));
    std::vector<std::string> metaLines;
    for (std::string line; std::getline(metaStream, line);) {
        metaLines.push_back(line);
    }
    REQUIRE(metaLines.size() == 5);
    CHECK(metaLines[0] == "--bridge-flag");
    CHECK(metaLines[1] == "bridge");
    CHECK(metaLines[2] == "ctx-exec");
    CHECK(metaLines[3] == "global-exec");
    CHECK(std::filesystem::exists(metaLines[4]));
    std::error_code removeError;
    std::filesystem::remove_all(metaLines[4], removeError);

    CHECK(output.find("[plugin] (bridge) debug-msg") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) info-msg") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) warn-msg") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) error-msg") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) status=17") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) progress=33%") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) begin_step: phase one") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) commit: committed") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) success: ok") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) failed: soft-fail") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) installed: demo-installed") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) listed: demo-listed") != std::string::npos);
    CHECK(output.find("[plugin] (bridge) artifact: artifact.json") != std::string::npos);
    CHECK(output.find("ctx-exec") != std::string::npos);
    CHECK(output.find("global-exec") != std::string::npos);
    CHECK(output.find("progress=41%") != std::string::npos);
    CHECK(output.find("line_progress: 41") != std::string::npos);
}

TEST_CASE("lua bridge exposes ordered repositories for current plugin", "[integration][lua_bridge][service]") {
    TempDir tempDir{"reqpack-lua-bridge-repositories"};
    ReqPackConfig config;
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins" / "maven";
    const std::filesystem::path scriptPath = pluginDirectory / "maven.lua";

    RepositoryEntry lowPriority;
    lowPriority.id = "corp";
    lowPriority.url = "https://repo.example.test/maven-public";
    lowPriority.priority = 1;
    lowPriority.auth.type = RepositoryAuthType::TOKEN;
    lowPriority.auth.token = "secret-token";
    lowPriority.validation.checksum = RepositoryChecksumPolicy::FAIL;
    lowPriority.validation.tlsVerify = false;
    lowPriority.scope.include = {"com.mycompany.*"};
    lowPriority.extras["snapshots"] = true;

    RepositoryEntry highPriority;
    highPriority.id = "central";
    highPriority.url = "https://repo1.maven.org/maven2";
    highPriority.priority = 20;
    highPriority.type = "default";
    highPriority.scope.exclude = {"com.mycompany.legacy.*"};
    highPriority.extras["tags"] = std::vector<std::string>{"public"};

    RepositoryEntry unrelated;
    unrelated.id = "pypi";
    unrelated.url = "https://pypi.org/simple";

    config.repositories["maven"] = {highPriority, lowPriority};
    config.repositories["pip"] = {unrelated};

    write_file(scriptPath, REPOSITORY_PLUGIN);

    LuaBridge bridge(scriptPath.string(), config);
    REQUIRE(bridge.init());

    const PackageInfo info = bridge.info(make_context(bridge, config), "demo");
    CHECK(info.name == "demo");
    CHECK(info.version == "2|corp|1|token|secret-token|fail|false|com.mycompany.*|true|central|default|com.mycompany.legacy.*|public");
    CHECK(info.description == "only-current-ecosystem");
}
