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

std::filesystem::path write_plugin_bundle(
    const std::filesystem::path& pluginRoot,
    const std::string& pluginName,
    const std::string& content,
    const std::vector<std::string>& dependencySpecs = {}
) {
    const std::filesystem::path pluginDirectory = pluginRoot / pluginName;
    std::string manifest = "return {\n  apiVersion = 1,\n  depends = {";
    if (!dependencySpecs.empty()) {
        manifest += "\n";
        for (const std::string& dependencySpec : dependencySpecs) {
            manifest += "    \"" + dependencySpec + "\",\n";
        }
        manifest += "  ";
    }
    manifest += "}\n}\n";

    write_file(pluginDirectory / "metadata.json",
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"" + pluginName + "\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"summary\": \"" + pluginName + " plugin\",\n"
        "  \"description\": \"" + pluginName + " plugin bundle\",\n"
        "  \"license\": \"MIT\"\n"
        "}\n");
    write_file(pluginDirectory / "reqpack.lua", manifest);
    write_file(pluginDirectory / "run.lua", content);
    write_file(pluginDirectory / "scripts" / "install.lua", "return true\n");
    write_file(pluginDirectory / "scripts" / "remove.lua", "return true\n");
    return pluginDirectory / "run.lua";
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

const char* STDERR_SUCCESS_CASE = R"(
return {
  name = "install success with stderr output",
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
      exitCode = 0,
      stdout = "ok\n",
      stderr = "repo sync\n",
    }
  },
  expect = {
    success = true,
    commands = { "fake-pm install curl" },
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

const char* DNF_PLUGIN = R"(
plugin = {}

local function trim(value)
  return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "rpm" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.outdated(context) return {} end

function plugin.list(context)
  local result = context.exec.run("dnf repoquery --installed --qf '%{name}.%{arch}\\t%{version}-%{release}\\n'")
  local summary = context.exec.run("dnf repoquery --installed --qf '%{name}.%{arch}\\t%{summary}\\n'")
  local summaryMap = {}
  for line in (summary.stdout or ""):gmatch("[^\r\n]+") do
    local qualifiedName, description = line:match("^(.-)\t(.+)$")
    if qualifiedName and description then
      summaryMap[trim(qualifiedName)] = trim(description)
    end
  end

  local items = {}
  for line in (result.stdout or ""):gmatch("[^\r\n]+") do
    local qualifiedName, version = line:match("^(.-)\t(.+)$")
    if qualifiedName and version then
      local name = trim(qualifiedName):match("^(.-)%.[^.]+$") or trim(qualifiedName)
      table.insert(items, { name = name, version = trim(version), description = summaryMap[trim(qualifiedName)] or "DNF package" })
    end
  end
  context.events.listed(items)
  return items
end

function plugin.search(context, prompt)
  local result = context.exec.run("dnf search '" .. prompt .. "' --quiet")
  local versions = context.exec.run("dnf repoquery --qf '%{name}.%{arch}\\t%{version}-%{release}\\n' '" .. prompt .. "' 2>/dev/null")
  local versionMap = {}
  for line in (versions.stdout or ""):gmatch("[^\r\n]+") do
    local qualifiedName, version = line:match("^(.-)\t(.+)$")
    if qualifiedName and version then
      local name = trim(qualifiedName):match("^(.-)%.[^.]+$") or trim(qualifiedName)
      versionMap[name] = trim(version)
    end
  end

  local items = {}
  for line in (result.stdout or ""):gmatch("[^\r\n]+") do
    local qualifiedName, description = line:match("^%s*(%S+)%s*\t%s*(.+)$")
    if qualifiedName and description and qualifiedName ~= "Matched" and qualifiedName ~= "Last" then
      local name = trim(qualifiedName):match("^(.-)%.[^.]+$") or trim(qualifiedName)
      table.insert(items, { name = name, version = versionMap[name] or "repo", description = trim(description) })
    end
  end
  context.events.searched(items)
  return items
end

function plugin.info(context, packageName)
  return { name = packageName, version = "1.0.0", description = "DNF package" }
end

function plugin.shutdown() return true end
)";

const char* DNF_LIST_CASE = R"(
return {
  name = "core list installed packages",
  request = {
    action = "list",
    system = "dnf",
  },
  fakeExec = {
    {
      match = "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{version}-%{release}\\n'",
      exitCode = 0,
      stdout = "curl.x86_64\t8.0-1\n",
      stderr = "",
      success = true,
    },
    {
      match = "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{summary}\\n'",
      exitCode = 0,
      stdout = "curl.x86_64\tTransfer tool\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    commands = {
      "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{version}-%{release}\\n'",
      "dnf repoquery --installed --qf '%{name}.%{arch}\\t%{summary}\\n'",
    },
    stdout = {
      "curl.x86_64\t8.0-1\n",
      "curl.x86_64\tTransfer tool\n",
    },
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0-1",
    events = { "listed" },
  }
}
)";

const char* DNF_SEARCH_CASE = R"(
return {
  name = "core search packages",
  request = {
    action = "search",
    system = "dnf",
    prompt = "curl",
  },
  fakeExec = {
    {
      match = "dnf search 'curl' --quiet",
      exitCode = 0,
      stdout = "Matched fields: name (exact)\n curl.x86_64\tTransfer tool\n",
      stderr = "",
      success = true,
    },
    {
      match = "dnf repoquery --qf '%{name}.%{arch}\\t%{version}-%{release}\\n' 'curl' 2>/dev/null",
      exitCode = 0,
      stdout = "curl.x86_64\t8.0-1\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0-1",
    events = { "searched" },
  }
}
)";

const char* MAVEN_PLUGIN = R"(
plugin = {}

local function trim(value)
  return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "maven" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.outdated(context) return {} end

local function pom_value(pomText, tagName)
  return trim((pomText or ""):match("<" .. tagName .. ">([\0-\255]-)</" .. tagName .. ">") or "")
end

local function list_local_artifacts(context)
  local repo = context.exec.run("printf '%s' \"${REQPACK_MAVEN_REPO:-}\"")
  local repoRoot = trim(repo.stdout or "")
  local poms = context.exec.run("find '" .. repoRoot .. "' -name '*.pom' 2>/dev/null")
  local items = {}
  for pomPath in (poms.stdout or ""):gmatch("[^\r\n]+") do
    local groupPath, artifactId, version = trim(pomPath):match("/([^/]+/[^/]+)/([^/]+)/([^/]+)/[^/]+%.pom$")
    if groupPath and artifactId and version then
      local groupId = groupPath:gsub("/", ".")
      local pom = context.exec.run("test -f '" .. trim(pomPath) .. "' && cat '" .. trim(pomPath) .. "'")
      table.insert(items, {
        name = groupId .. ":" .. artifactId,
        version = version,
        description = pom_value(pom.stdout or "", "description"),
      })
    end
  end
  return items
end

function plugin.list(context)
  local items = list_local_artifacts(context)
  context.events.listed(items)
  return items
end

function plugin.search(context, prompt)
  local items = {}
  for _, item in ipairs(list_local_artifacts(context)) do
    if item.name == prompt then
      table.insert(items, item)
    end
  end
  context.events.searched(items)
  return items
end

function plugin.info(context, packageName)
  return { name = packageName, version = "1.0.0", description = "Maven artifact" }
end

function plugin.shutdown() return true end
)";

const char* MAVEN_LIST_CASE = R"(
return {
  name = "core list local artifacts",
  request = {
    action = "list",
    system = "maven",
  },
  fakeExec = {
    {
      match = "printf '%s' \"${REQPACK_MAVEN_REPO:-}\"",
      exitCode = 0,
      stdout = "/tmp/m2\n",
      stderr = "",
      success = true,
    },
    {
      match = "find '/tmp/m2' -name '*.pom' 2>/dev/null",
      exitCode = 0,
      stdout = "/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom\n",
      stderr = "",
      success = true,
    },
    {
      match = "test -f '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom' && cat '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom'",
      exitCode = 0,
      stdout = "<project><packaging>jar</packaging><name>Demo Lib</name><description>Demo artifact</description></project>",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "org.example:demo-lib",
    resultVersion = "1.2.3",
    events = { "listed" },
  }
}
)";

const char* MAVEN_SEARCH_CASE = R"(
return {
  name = "core search local or central artifacts",
  request = {
    action = "search",
    system = "maven",
    prompt = "org.example:demo-lib",
  },
  fakeExec = {
    {
      match = "printf '%s' \"${REQPACK_MAVEN_REPO:-}\"",
      exitCode = 0,
      stdout = "/tmp/m2\n",
      stderr = "",
      success = true,
    },
    {
      match = "find '/tmp/m2' -name '*.pom' 2>/dev/null",
      exitCode = 0,
      stdout = "/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom\n",
      stderr = "",
      success = true,
    },
    {
      match = "test -f '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom' && cat '/tmp/m2/org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom'",
      exitCode = 0,
      stdout = "<project><packaging>jar</packaging><name>Demo Lib</name><description>Demo artifact</description></project>",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "org.example:demo-lib",
    resultVersion = "1.2.3",
    events = { "searched" },
  }
}
)";

const char* SYS_PLUGIN = R"(
plugin = {}

local function trim(value)
  return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "sys" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.outdated(context) return {} end

function plugin.list(context)
  local backend = context.exec.run("printf '%s' \"${REQPACK_SYS_BACKEND:-}\"")
  local backendName = trim(backend.stdout or "")
  if backendName ~= "apt" then
    return {}
  end
  local query = context.exec.run("printf '%s' \"${REQPACK_SYS_DPKG_QUERY_BIN:-}\"")
  local bin = trim(query.stdout or "")
  context.exec.run("test -x '" .. bin .. "'")
  local listed = context.exec.run("'" .. bin .. "' -W -f='${Package}\\t${Version}\\n'")
  local items = {}
  for line in (listed.stdout or ""):gmatch("[^\r\n]+") do
    local name, version = line:match("^(.-)\t(.+)$")
    if name and version then
      table.insert(items, { name = trim(name), version = trim(version), description = "System package" })
    end
  end
  context.events.listed(items)
  return items
end

function plugin.search(context, prompt)
  local backend = context.exec.run("printf '%s' \"${REQPACK_SYS_BACKEND:-}\"")
  local backendName = trim(backend.stdout or "")
  if backendName ~= "apt" then
    return {}
  end
  local cache = context.exec.run("printf '%s' \"${REQPACK_SYS_APT_CACHE_BIN:-}\"")
  local bin = trim(cache.stdout or "")
  local searched = context.exec.run("'" .. bin .. "' search '" .. prompt .. "'")
  local items = {}
  for line in (searched.stdout or ""):gmatch("[^\r\n]+") do
    local name, description = line:match("^(.-) %- (.+)$")
    if name and description then
      table.insert(items, { name = trim(name), version = "repo", description = trim(description) })
    end
  end
  context.events.searched(items)
  return items
end

function plugin.info(context, packageName)
  return { name = packageName, version = "repo", description = "System package" }
end

function plugin.shutdown() return true end
)";

const char* SYS_LIST_CASE = R"(
return {
  name = "core list apt packages",
  request = {
    action = "list",
    system = "sys",
  },
  fakeExec = {
    {
      match = "printf '%s' \"${REQPACK_SYS_BACKEND:-}\"",
      exitCode = 0,
      stdout = "apt",
      stderr = "",
      success = true,
    },
    {
      match = "printf '%s' \"${REQPACK_SYS_DPKG_QUERY_BIN:-}\"",
      exitCode = 0,
      stdout = "/usr/bin/dpkg-query",
      stderr = "",
      success = true,
    },
    {
      match = "test -x '/usr/bin/dpkg-query'",
      exitCode = 0,
      stdout = "",
      stderr = "",
      success = true,
    },
    {
      match = "'/usr/bin/dpkg-query' -W -f='${Package}\\t${Version}\\n'",
      exitCode = 0,
      stdout = "curl\t8.0\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "8.0",
    events = { "listed" },
  }
}
)";

const char* SYS_SEARCH_CASE = R"(
return {
  name = "core search apt packages",
  request = {
    action = "search",
    system = "sys",
    prompt = "curl",
  },
  fakeExec = {
    {
      match = "printf '%s' \"${REQPACK_SYS_BACKEND:-}\"",
      exitCode = 0,
      stdout = "apt",
      stderr = "",
      success = true,
    },
    {
      match = "printf '%s' \"${REQPACK_SYS_APT_CACHE_BIN:-}\"",
      exitCode = 0,
      stdout = "/usr/bin/apt-cache",
      stderr = "",
      success = true,
    },
    {
      match = "'/usr/bin/apt-cache' search 'curl'",
      exitCode = 0,
      stdout = "curl - Transfer tool\n",
      stderr = "",
      success = true,
    },
  },
  expect = {
    success = true,
    resultCount = 1,
    resultName = "curl",
    resultVersion = "repo",
    events = { "searched" },
  }
}
)";

const char* JAVA_PROXY_PLUGIN = R"(
plugin = {}

local function trim(value)
  return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function normalized_target(value)
  local target = trim(value):lower()
  if target == "" then
    return nil
  end
  return target
end

local function choose_target(context)
  local proxy = context.proxy or {}
  local target = normalized_target(proxy.default)
  if target ~= nil then
    return target
  end
  local targets = proxy.targets or {}
  for _, candidate in ipairs(targets) do
    target = normalized_target(candidate)
    if target ~= nil then
      return target
    end
  end
  return "maven"
end

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "proxy", "java" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return false end
function plugin.installLocal(context, path) return false end
function plugin.remove(context, packages) return false end
function plugin.update(context, packages) return false end
function plugin.list(context) return {} end
function plugin.outdated(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  local item = {
    name = package or "java",
    version = "proxy",
    description = "Java proxy manager",
  }
  context.events.informed(item)
  return item
end
function plugin.resolveProxyRequest(context, request)
  return {
    targetSystem = choose_target(context),
    packages = request.packages,
    flags = request.flags,
  }
end
function plugin.shutdown() return true end
)";

const char* JAVA_PROXY_CASE = R"(
return {
  name = "core proxy request resolves target",
  request = {
    action = "info",
    system = "java",
    prompt = "demo-artifact",
  },
  fakeExec = {},
  expect = {
    success = true,
    resultCount = 1,
    resultName = "demo-artifact",
    resultVersion = "proxy",
    events = { "informed" },
    eventPayloads = {
      informed = "{description=Java proxy manager, name=demo-artifact, version=proxy}",
    },
  }
}
)";

void write_preset_cases(const std::filesystem::path& pluginRoot, const std::string& pluginName, const std::vector<std::pair<std::string, std::string>>& cases) {
    const std::filesystem::path presetDirectory = pluginRoot / pluginName / ".reqpack-test" / "core";
    for (const auto& [fileName, content] : cases) {
        write_file(presetDirectory / fileName, content);
    }
}

void write_dnf_plugin_fixture(const std::filesystem::path& pluginRoot) {
    write_plugin_bundle(pluginRoot, "dnf", DNF_PLUGIN);
    write_preset_cases(pluginRoot, "dnf", {{"list.lua", DNF_LIST_CASE}, {"search.lua", DNF_SEARCH_CASE}});
}

void write_maven_plugin_fixture(const std::filesystem::path& pluginRoot) {
    write_plugin_bundle(pluginRoot, "maven", MAVEN_PLUGIN, {"sys:java", "sys:maven"});
    write_preset_cases(pluginRoot, "maven", {{"list.lua", MAVEN_LIST_CASE}, {"search.lua", MAVEN_SEARCH_CASE}});
}

void write_sys_plugin_fixture(const std::filesystem::path& pluginRoot) {
    write_plugin_bundle(pluginRoot, "sys", SYS_PLUGIN);
    write_preset_cases(pluginRoot, "sys", {{"list.lua", SYS_LIST_CASE}, {"search.lua", SYS_SEARCH_CASE}});
}

void write_java_plugin_fixture(const std::filesystem::path& pluginRoot) {
    write_plugin_bundle(pluginRoot, "java", JAVA_PROXY_PLUGIN);
    write_preset_cases(pluginRoot, "java", {{"proxy.lua", JAVA_PROXY_CASE}});
}

}  // namespace

TEST_CASE("plugin test command runs hermetic case and writes report", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-pass"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    const std::filesystem::path reportPath = tempDir.path() / "report.json";
    write_plugin_bundle(plugins, "demo", TEST_PLUGIN);
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

TEST_CASE("plugin test command treats zero-exit stderr output as success by default", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-stderr-success"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    write_plugin_bundle(plugins, "demo", TEST_PLUGIN);
    write_file(cases / "install_stderr_success.lua", STDERR_SUCCESS_CASE);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "demo", "--case", (cases / "install_stderr_success.lua").string()
    });

    CHECK(output.find("[PASS] install success with stderr output") != std::string::npos);
    CHECK(output.find("Cases: 1, Passed: 1, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command loads cases from directory and returns failures", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-dir"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    write_plugin_bundle(plugins, "demo", TEST_PLUGIN);
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
    const bool reportedCaseName = output.find("install expectation mismatch") != std::string::npos;
    const bool reportedFileName = output.find("b-fail") != std::string::npos;
    const bool reportedFailureLabel = reportedCaseName || reportedFileName;

    INFO(output);
    CHECK(output.find("[PASS] install success") != std::string::npos);
    CHECK(output.find("[FAIL]") != std::string::npos);
    CHECK(reportedFailureLabel);
    CHECK(output.find("expected success=true") != std::string::npos);
    CHECK(output.find("Cases: 2, Passed: 1, Failed: 1") != std::string::npos);
    CHECK(output.find("EXIT:1") != std::string::npos);
}

TEST_CASE("plugin test command supports query cases and help", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-help"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    const std::filesystem::path cases = tempDir.path() / "cases";
    write_plugin_bundle(plugins, "demo", TEST_PLUGIN);
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
    write_plugin_bundle(plugins, "demo", TEST_PLUGIN);
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
    write_dnf_plugin_fixture(plugins);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "dnf", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo maven preset cases", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-maven-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    write_maven_plugin_fixture(plugins);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "maven", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo sys preset cases", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-sys-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    write_sys_plugin_fixture(plugins);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "sys", "--preset", "core"
    });

    CHECK(output.find("Cases: 2, Passed: 2, Failed: 0") != std::string::npos);
}

TEST_CASE("plugin test command runs repo java preset case", "[integration][plugin-test][service]") {
    TempDir tempDir{"reqpack-plugin-test-java-preset"};
    const std::filesystem::path plugins = tempDir.path() / "plugins";
    write_java_plugin_fixture(plugins);
    const std::filesystem::path configPath = write_config(tempDir.path(), plugins);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "test-plugin", "--plugin", "java", "--preset", "core"
    });

    CHECK(output.find("Cases: 1, Passed: 1, Failed: 0") != std::string::npos);
}
