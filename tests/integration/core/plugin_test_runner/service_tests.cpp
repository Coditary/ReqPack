#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

std::filesystem::path write_config(const std::filesystem::path& root, const std::filesystem::path& pluginDirectory) {
    const std::filesystem::path configPath = root / "config.lua";
    write_file(configPath,
        "return {\n"
        "  security = { autoFetch = false },\n"
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
        "  interaction = { interactive = false },\n"
        "}\n");
    return configPath;
}

void copy_repo_plugin(const std::filesystem::path& pluginRoot, const std::string& pluginName) {
    const std::filesystem::path repoPluginDir = repo_root() / "plugins" / pluginName;
    write_file(pluginRoot / pluginName / (pluginName + ".lua"), read_file(repoPluginDir / (pluginName + ".lua")));

    const std::filesystem::path bootstrapPath = repoPluginDir / "bootstrap.lua";
    if (std::filesystem::exists(bootstrapPath)) {
        write_file(pluginRoot / pluginName / "bootstrap.lua", read_file(bootstrapPath));
    }

    const std::filesystem::path presetRoot = repoPluginDir / ".reqpack-test";
    if (!std::filesystem::exists(presetRoot)) {
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(presetRoot)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), repoPluginDir);
        write_file(pluginRoot / pluginName / relative, read_file(entry.path()));
    }
}

std::string run_reqpack(const std::filesystem::path& workspace, const std::filesystem::path& configPath, const std::vector<std::string>& arguments) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && " + escape_shell_arg((build_root() / "rqp").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

const char* TEST_PLUGIN = R"(
plugin = {}

function plugin.getName() return "plugin-test" end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "test" } end
function plugin.getMissingPackages(packages) return packages end

function plugin.install(context, packages)
  context.tx.begin_step("install")
  local result = context.exec.run("fake-pm install " .. packages[1].name)
  if not result.success then
    context.tx.failed("install failed")
    return false
  end
  context.events.installed({ name = packages[1].name })
  context.tx.success()
  return true
end

function plugin.installLocal(context, path)
  local result = context.exec.run("fake-pm install-local " .. path)
  return result.success
end

function plugin.remove(context, packages)
  local result = context.exec.run("fake-pm remove " .. packages[1].name)
  if result.success then
    context.events.deleted({ name = packages[1].name })
  end
  return result.success
end

function plugin.update(context, packages)
  local result = context.exec.run("fake-pm update " .. packages[1].name)
  if result.success then
    context.events.updated({ name = packages[1].name })
  end
  return result.success
end

function plugin.list(context)
  local result = reqpack.exec.run("fake-pm list")
  if not result.success then
    return {}
  end
  return {
    { name = "alpha", version = "1.2.3", description = result.stdout },
  }
end

function plugin.search(context, prompt)
  local result = context.exec.run("fake-pm search " .. prompt)
  if not result.success then
    return {}
  end
  context.artifacts.register({ kind = "search-cache", path = "/tmp/search-cache.json" })
  return {
    { name = prompt, version = "9.9.9", description = result.stdout },
  }
end

function plugin.info(context, packageName)
  local result = context.exec.run("fake-pm info " .. packageName)
  if not result.success then
    return {}
  end
  return { name = packageName, version = "4.5.6", description = result.stdout }
end
)";

const char* PASS_CASE = R"(
return {
  name = "install success",
  request = {
    action = "install",
    system = "demo",
    packages = {
      { name = "curl", version = "8.0" }
    }
  },
  fakeExec = {
    {
      match = "fake-pm install curl",
      exitCode = 0,
      stdout = "ok\n",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm install curl" },
    stdout = { "ok\n" },
    events = { "installed", "success" },
    eventPayloads = {
      installed = "{name=curl}",
      success = "ok",
    },
  }
}
)";

const char* FAIL_CASE = R"(
return {
  name = "install expectation mismatch",
  request = {
    action = "install",
    system = "demo",
    packages = {
      { name = "curl" }
    }
  },
  fakeExec = {
    {
      match = "fake-pm install curl",
      exitCode = 1,
      stdout = "",
      stderr = "boom",
      success = false,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm install curl" },
  }
}
)";

const char* LIST_CASE = R"(
return {
  name = "list success",
  request = {
    action = "list",
    system = "demo"
  },
  fakeExec = {
    {
      match = "fake-pm list",
      exitCode = 0,
      stdout = "alpha line",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm list" },
    stdout = { "alpha line" },
    resultCount = 1,
    resultName = "alpha",
    resultVersion = "1.2.3",
  }
}
)";

const char* SEARCH_CASE = R"(
return {
  name = "search artifact and payload",
  request = {
    action = "search",
    system = "demo",
    prompt = "delta"
  },
  fakeExec = {
    {
      match = "fake-pm search delta",
      exitCode = 0,
      stdout = "delta line",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm search delta" },
    stdout = { "delta line" },
    artifacts = { "{kind=search-cache, path=/tmp/search-cache.json}" },
    resultCount = 1,
    resultName = "delta",
    resultVersion = "9.9.9",
  }
}
)";

const char* PRESET_LIST_CASE = R"(
return {
  name = "core list preset",
  request = {
    action = "list",
    system = "demo"
  },
  fakeExec = {
    {
      match = "fake-pm list",
      exitCode = 0,
      stdout = "alpha line",
      stderr = "",
      success = true,
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm list" },
    resultCount = 1,
    resultName = "alpha",
  }
}
)";

}  // namespace

TEST_CASE("plugin test command runs hermetic case and writes report", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-pass"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    const std::filesystem::path reportPath = tempDir.path() / "report.json";
    write_file(plugins / "demo" / "demo.lua", TEST_PLUGIN);
    write_file(cases / "install_pass.lua", PASS_CASE);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "demo", "--case", (cases / "install_pass.lua").string(), "--report", reportPath.string()
    });

    CHECK(output.find("[PASS] install success") != std::string::npos);
    CHECK(output.find("Cases: 1, Passed: 1, Failed: 0") != std::string::npos);
    const std::string report = read_file(reportPath);
    CHECK(report.find("\"plugin\": \"demo\"") != std::string::npos);
    CHECK(report.find("\"success\": true") != std::string::npos);
    CHECK(report.find("\"stdout\": [\"ok\\n\"]") != std::string::npos);
    CHECK(report.find("\"eventRecords\": [{\"name\":\"installed\",\"payload\":\"{name=curl}\"}") != std::string::npos);
}

TEST_CASE("plugin test command loads cases from directory and returns failures", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-dir"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    write_file(plugins / "demo" / "demo.lua", TEST_PLUGIN);
    write_file(cases / "a-pass.lua", PASS_CASE);
    write_file(cases / "b-fail.lua", FAIL_CASE);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string command =
        "cd " + escape_shell_arg(tempDir.path().string()) +
        " && " + escape_shell_arg((build_root() / "rqp").string()) +
        " --config " + escape_shell_arg(configPath.string()) +
        " test-plugin --plugin demo --cases " + escape_shell_arg(cases.string()) +
        " 2>&1; printf '\nEXIT:%s' \"$?\"";
    const std::string output = run_command_capture(command);

    CHECK(output.find("[PASS] install success") != std::string::npos);
    CHECK(output.find("[FAIL] install expectation mismatch") != std::string::npos);
    CHECK(output.find("Cases: 2, Passed: 1, Failed: 1") != std::string::npos);
    CHECK(output.find("EXIT:1") != std::string::npos);
}

TEST_CASE("plugin test command supports query cases and help", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-help"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    write_file(plugins / "demo" / "demo.lua", TEST_PLUGIN);
    write_file(cases / "list.lua", LIST_CASE);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", (plugins / "demo").string(), "--case", (cases / "list.lua").string()
    });
    CHECK(output.find("[PASS] list success") != std::string::npos);

    const std::string helpOutput = run_reqpack(tempDir.path(), configPath, {"test-plugin", "--help"});
    CHECK(helpOutput.find("Run hermetic plugin conformance cases") != std::string::npos);
    CHECK(helpOutput.find("--plugin <value>") != std::string::npos);
    CHECK(helpOutput.find("--preset <name>") != std::string::npos);
}

TEST_CASE("plugin test command validates artifacts and supports presets", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    const std::filesystem::path presetDir = plugins / "demo" / ".reqpack-test" / "core";
    write_file(plugins / "demo" / "demo.lua", TEST_PLUGIN);
    write_file(cases / "search.lua", SEARCH_CASE);
    write_file(presetDir / "list.lua", PRESET_LIST_CASE);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "demo", "--preset", "core", "--case", (cases / "search.lua").string()
    });

    CHECK(output.find("[PASS] core list preset") != std::string::npos);
    CHECK(output.find("[PASS] search artifact and payload") != std::string::npos);
    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo dnf preset cases", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-dnf-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    copy_repo_plugin(plugins, "dnf");
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "dnf", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo maven preset cases", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-maven-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    copy_repo_plugin(plugins, "maven");
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "maven", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo sys preset cases", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-sys-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    copy_repo_plugin(plugins, "sys");
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "sys", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo java preset case", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-java-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    copy_repo_plugin(plugins, "java");
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "java", "--preset", "core"
    });

    CHECK(output.find("Cases: 1, Passed: 1, Failed: 0") != std::string::npos);
}
