#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <optional>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch.hpp>

#include "test_helpers.h"

extern char** environ;

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

void copy_repo_plugin(const std::filesystem::path& pluginRoot, const std::string& pluginName) {
    const std::filesystem::path repoPluginDir = repo_root() / "plugins" / pluginName;
    write_file(pluginRoot / pluginName / (pluginName + ".lua"), read_file(repoPluginDir / (pluginName + ".lua")));
    const std::filesystem::path bootstrapPath = repoPluginDir / "bootstrap.lua";
    if (std::filesystem::exists(bootstrapPath)) {
        write_file(pluginRoot / pluginName / "bootstrap.lua", read_file(bootstrapPath));
    }
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
        "  rqp = {\n"
        "    statePath = '" + (root / "rqp-state").string() + "',\n"
        "  },\n"
        "}\n");
    return configPath;
}

std::filesystem::path write_config_with_rq_repositories(
    const std::filesystem::path& root,
    const std::filesystem::path& pluginDirectory,
    const std::vector<std::string>& repositories
) {
    const std::filesystem::path configPath = root / "config.lua";
    std::string repositoryList = "";
    for (const std::string& repository : repositories) {
        repositoryList += "      '" + repository + "',\n";
    }
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
        "  rqp = {\n"
        "    statePath = '" + (root / "rqp-state").string() + "',\n"
        "    repositories = {\n" + repositoryList +
        "    },\n"
        "  },\n"
        "}\n");
    return configPath;
}

std::filesystem::path build_rqp_package(
    const std::filesystem::path& root,
    const std::string& name,
    const std::string& installLua,
    const std::optional<std::pair<std::string, std::string>>& payloadFile = std::nullopt,
    const std::optional<std::string>& overrideHash = std::nullopt,
    const std::string& version = "1.0.0"
) {
    const std::filesystem::path packageRoot = root / (name + "-pkg");
    const std::filesystem::path payloadRoot = packageRoot / "payload-tree";
    const std::filesystem::path controlRoot = packageRoot / "control";
    std::filesystem::create_directories(controlRoot / "scripts");
    std::filesystem::create_directories(controlRoot / "hashes");
    std::filesystem::create_directories(controlRoot / "payload");

    if (payloadFile.has_value()) {
        const std::filesystem::path payloadPath = payloadRoot / payloadFile->first;
        write_file(payloadPath, payloadFile->second);
        const std::string payloadTar = (controlRoot / "payload" / "payload.tar").string();
        const std::string payloadTarZst = (controlRoot / "payload" / "payload.tar.zst").string();
        const std::string tarCommand = "tar -C " + escape_shell_arg(payloadRoot.string()) + " -cf " + escape_shell_arg(payloadTar) + " .";
        REQUIRE(std::system(tarCommand.c_str()) == 0);
        const std::string zstdCommand = "zstd -q -f " + escape_shell_arg(payloadTar) + " -o " + escape_shell_arg(payloadTarZst);
        REQUIRE(std::system(zstdCommand.c_str()) == 0);
        std::string hash = overrideHash.value_or({});
        if (!overrideHash.has_value()) {
            const std::string hashOutput = run_command_capture("openssl dgst -sha256 " + escape_shell_arg(payloadTarZst));
            const std::size_t pos = hashOutput.rfind(' ');
            REQUIRE(pos != std::string::npos);
            hash = hashOutput.substr(pos + 1, 64);
        }
        write_file(controlRoot / "hashes" / "payload.sha256", hash + "  payload/payload.tar.zst\n");
        std::error_code removeError;
        std::filesystem::remove(controlRoot / "payload" / "payload.tar", removeError);
    }

    const std::string metadata = payloadFile.has_value()
        ? "{\n"
          "  \"formatVersion\": 1,\n"
          "  \"name\": \"" + name + "\",\n"
          "  \"version\": \"" + version + "\",\n"
          "  \"release\": 1,\n"
          "  \"revision\": 0,\n"
          "  \"summary\": \"test package\",\n"
          "  \"description\": \"integration test package\",\n"
          "  \"license\": \"MIT\",\n"
          "  \"architecture\": \"noarch\",\n"
          "  \"vendor\": \"ReqPack Tests\",\n"
          "  \"maintainerEmail\": \"tests@example.org\",\n"
          "  \"tags\": [\"test\"],\n"
          "  \"url\": \"https://example.test/" + name + ".rqp\",\n"
          "  \"payload\": {\n"
          "    \"path\": \"payload/payload.tar.zst\",\n"
          "    \"archive\": \"tar\",\n"
          "    \"compression\": \"zstd\",\n"
          "    \"hashAlgorithm\": \"sha256\",\n"
          "    \"hashFile\": \"hashes/payload.sha256\",\n"
          "    \"sizeCompressed\": 0,\n"
          "    \"sizeInstalledExpected\": 0\n"
          "  }\n"
          "}\n"
        : "{\n"
          "  \"formatVersion\": 1,\n"
          "  \"name\": \"" + name + "\",\n"
          "  \"version\": \"" + version + "\",\n"
          "  \"release\": 1,\n"
          "  \"revision\": 0,\n"
          "  \"summary\": \"test package\",\n"
          "  \"description\": \"integration test package\",\n"
          "  \"license\": \"MIT\",\n"
          "  \"architecture\": \"noarch\",\n"
          "  \"vendor\": \"ReqPack Tests\",\n"
          "  \"maintainerEmail\": \"tests@example.org\",\n"
          "  \"tags\": [\"test\"],\n"
          "  \"url\": \"https://example.test/" + name + ".rqp\"\n"
          "}\n";

    write_file(controlRoot / "metadata.json", metadata);
    write_file(controlRoot / "reqpack.lua", R"(
return {
  apiVersion = 1,
  hooks = {
    install = "scripts/install.lua"
  }
}
)");
    write_file(controlRoot / "scripts" / "install.lua", installLua);

    const std::filesystem::path packagePath = root / (name + ".rqp");
    const std::string tarCommand = "tar -C " + escape_shell_arg(controlRoot.string()) + " -cf " + escape_shell_arg(packagePath.string()) + " .";
    REQUIRE(std::system(tarCommand.c_str()) == 0);
    return packagePath;
}

std::string sha256_file_hex(const std::filesystem::path& path) {
    const std::string hashOutput = run_command_capture("openssl dgst -sha256 " + escape_shell_arg(path.string()));
    const std::size_t pos = hashOutput.rfind(' ');
    REQUIRE(pos != std::string::npos);
    return hashOutput.substr(pos + 1, 64);
}

std::filesystem::path write_rq_repository_index(
    const std::filesystem::path& root,
    const std::string& packageName,
    const std::string& packageVersion,
    const std::filesystem::path& artifactPath,
    const std::optional<std::string>& packageSha256 = std::nullopt
) {
    const std::filesystem::path indexPath = root / "index.json";
    write_file(indexPath,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"packages\": [\n"
        "    {\n"
        "      \"name\": \"" + packageName + "\",\n"
        "      \"version\": \"" + packageVersion + "\",\n"
        "      \"release\": 1,\n"
        "      \"revision\": 0,\n"
        "      \"architecture\": \"noarch\",\n"
        "      \"summary\": \"repo package\",\n"
        "      \"url\": \"file://" + artifactPath.string() + "\""
        + (packageSha256.has_value() ? ",\n      \"packageSha256\": \"" + packageSha256.value() + "\"\n" : "\n") +
        "    }\n"
        "  ]\n"
        "}\n");
    return indexPath;
}

std::filesystem::path write_remote_profiles(const std::filesystem::path& root, const std::string& content) {
    const std::filesystem::path reqpackHome = root / ".reqpack";
    std::filesystem::create_directories(reqpackHome);
    const std::filesystem::path profilePath = reqpackHome / "remote.lua";
    write_file(profilePath, content);
    return profilePath;
}

std::filesystem::path write_remote_users(const std::filesystem::path& root, const std::string& content) {
    return write_remote_profiles(root, content);
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

std::string run_reqpack_with_home(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::string>& arguments
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && HOME=" + escape_shell_arg(homePath.string()) +
        " " + escape_shell_arg((build_root() / "ReqPack").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

std::string run_reqpack_with_stdin(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::vector<std::string>& arguments,
    const std::string& stdinContent
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && printf %s " + escape_shell_arg(stdinContent) +
        " | " + escape_shell_arg((build_root() / "ReqPack").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

class ServerProcess {
public:
    ServerProcess(
        const std::filesystem::path& workspace,
        const std::filesystem::path& configPath,
        const std::optional<std::filesystem::path>& homePath,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& logPath
    ) {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0 ||
            posix_spawn_file_actions_addchdir_np(&actions, workspace.c_str()) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644) != 0) {
            throw std::runtime_error("failed to configure server process actions");
        }

        std::vector<std::string> argvStrings;
        argvStrings.push_back((build_root() / "ReqPack").string());
        argvStrings.push_back("--config");
        argvStrings.push_back(configPath.string());
        argvStrings.insert(argvStrings.end(), arguments.begin(), arguments.end());

        std::vector<std::string> environmentStrings;
        std::vector<char*> environment;
        if (homePath.has_value()) {
            environmentStrings.push_back("HOME=" + homePath->string());
            environment.reserve(environmentStrings.size() + 1);
            for (std::string& value : environmentStrings) {
                environment.push_back(value.data());
            }
            environment.push_back(nullptr);
        }

        std::vector<char*> argv;
        argv.reserve(argvStrings.size() + 1);
        for (std::string& argument : argvStrings) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        const int spawnResult = posix_spawn(
            &pid_,
            argvStrings.front().c_str(),
            &actions,
            nullptr,
            argv.data(),
            homePath.has_value() ? environment.data() : environ
        );
        posix_spawn_file_actions_destroy(&actions);
        if (spawnResult != 0) {
            throw std::runtime_error("failed to spawn server process");
        }
    }

    ~ServerProcess() {
        stop();
    }

    void stop() {
        if (pid_ <= 0) {
            return;
        }
        (void)::kill(pid_, SIGTERM);
        (void)::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

int reserve_tcp_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        throw std::runtime_error("failed to create port reservation socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        throw std::runtime_error("failed to bind port reservation socket");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        throw std::runtime_error("failed to inspect reserved port");
    }
    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

int connect_with_retry(const std::string& host, int port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            continue;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            ::close(fd);
            throw std::runtime_error("failed to parse test host address");
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FAIL("failed to connect to server");
    throw std::runtime_error("failed to connect to server");
}

bool send_socket_text(int fd, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::send(fd, text.data() + offset, text.size() - offset, 0);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::string read_socket_line(int fd) {
    std::string line;
    char c = '\0';
    while (::recv(fd, &c, 1, 0) == 1) {
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
    return line;
}

std::string read_socket_bytes(int fd, std::size_t count) {
    std::string out(count, '\0');
    std::size_t offset = 0;
    while (offset < count) {
        const ssize_t received = ::recv(fd, out.data() + offset, count - offset, 0);
        if (received <= 0) {
            throw std::runtime_error("failed to read expected socket payload");
        }
        offset += static_cast<std::size_t>(received);
    }
    return out;
}

std::pair<std::string, std::string> read_text_protocol_response(int fd) {
    const std::string header = read_socket_line(fd);
    const std::size_t separator = header.find(' ');
    if (separator == std::string::npos) {
        throw std::runtime_error("invalid text protocol response header");
    }
    const std::string status = header.substr(0, separator);
    const std::size_t length = static_cast<std::size_t>(std::stoul(header.substr(separator + 1)));
    return {status, read_socket_bytes(fd, length)};
}

std::string read_json_response_line(int fd) {
    return read_socket_line(fd);
}

std::string run_reqpack_with_home_and_status(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::string>& arguments,
    int& status
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && HOME=" + escape_shell_arg(homePath.string()) +
        " " + escape_shell_arg((build_root() / "ReqPack").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run command: " + command);
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    status = pclose(pipe);
    if (status == -1) {
        throw std::runtime_error("failed to close command pipe: " + command);
    }
    return output;
}

std::string run_reqpack_with_home_env_and_status(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::vector<std::string>& arguments,
    int& status
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && env HOME=" + escape_shell_arg(homePath.string());
    for (const auto& [key, value] : environment) {
        command += " " + key + "=" + escape_shell_arg(value);
    }
    command += " " + escape_shell_arg((build_root() / "ReqPack").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";

    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to run command: " + command);
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    status = pclose(pipe);
    if (status == -1) {
        throw std::runtime_error("failed to close command pipe: " + command);
    }
    return output;
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

TEST_CASE("orchestrator install local rqp resolves to built-in rqp by extension", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-rqp"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-rqp"))
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", localPackage.string()});
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "artifact" / "artifact@1.0.0-1+r0";
    INFO(output);

    CHECK(output.find("INSTALL: rqp:local") != std::string::npos);
    CHECK(output.find("1 ok") != std::string::npos);
    CHECK(std::filesystem::exists(stateDir / "installed.txt"));
    CHECK(std::filesystem::exists(stateDir / "metadata.json"));
    CHECK(std::filesystem::exists(stateDir / "reqpack.lua"));
    CHECK(std::filesystem::exists(stateDir / "manifest.json"));
}

TEST_CASE("orchestrator install named rqp package resolves from repository index", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-rqp-repo"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path artifactPath = build_rqp_package(
        tempDir.path(),
        "repo-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-from-repo"))
    );
    const std::filesystem::path indexPath = write_rq_repository_index(
        tempDir.path(),
        "repo-artifact",
        "1.0.0",
        artifactPath,
        sha256_file_hex(artifactPath)
    );
    const std::filesystem::path configPath = write_config_with_rq_repositories(
        tempDir.path(),
        pluginDirectory,
        {"file://" + indexPath.string()}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "rqp:repo-artifact@1.0.0"});
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "repo-artifact" / "repo-artifact@1.0.0-1+r0";

    CHECK(output.find("INSTALL: rqp:repo-artifact") != std::string::npos);
    CHECK(output.find("1 ok") != std::string::npos);
    CHECK(std::filesystem::exists(stateDir / "installed.txt"));
    CHECK(read_file(stateDir / "installed.txt") == "hello-from-repo");
    CHECK(read_file(stateDir / "source.json").find("\"source\": \"repository\"") != std::string::npos);
}

TEST_CASE("orchestrator install rqp package aborts on repository artifact hash mismatch", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-rqp-repo-bad-hash"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path artifactPath = build_rqp_package(
        tempDir.path(),
        "repo-bad-hash",
        "return true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-from-repo"))
    );
    const std::filesystem::path indexPath = write_rq_repository_index(
        tempDir.path(),
        "repo-bad-hash",
        "1.0.0",
        artifactPath,
        std::string(64, 'a')
    );
    const std::filesystem::path configPath = write_config_with_rq_repositories(
        tempDir.path(),
        pluginDirectory,
        {"file://" + indexPath.string()}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "rqp", "repo-bad-hash@1.0.0"});
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "repo-bad-hash" / "repo-bad-hash@1.0.0-1+r0";

    CHECK(output.find("rqp:repo-bad-hash") != std::string::npos);
    CHECK(output.find("repository package sha256 mismatch") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(stateDir));
}

TEST_CASE("orchestrator list and info read installed rqp state", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-rqp-list-info"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "listed-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-list"))
    );

    (void)run_reqpack(tempDir.path(), configPath, {"install", localPackage.string()});

    const std::string listOutput = run_reqpack(tempDir.path(), configPath, {"list", "rqp"});
    const std::string infoOutput = run_reqpack(tempDir.path(), configPath, {"info", "rqp", "listed-artifact"});

    CHECK(listOutput.find("listed-artifact 1.0.0-1+r0 - test package") != std::string::npos);
    CHECK(infoOutput.find("listed-artifact 1.0.0-1+r0 - test package") != std::string::npos);
}

TEST_CASE("orchestrator remove rqp package deletes state and artifacts", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-rqp-remove"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "removable-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\ncontext.artifacts.register_file(out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-remove"))
    );
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "removable-artifact" / "removable-artifact@1.0.0-1+r0";

    (void)run_reqpack(tempDir.path(), configPath, {"install", localPackage.string()});
    REQUIRE(std::filesystem::exists(stateDir / "installed.txt"));

    const std::string output = run_reqpack(tempDir.path(), configPath, {"remove", "rqp", "removable-artifact@1.0.0"});

    CHECK(output.find("REMOVE: rqp:removable-artifact") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(stateDir));
}

TEST_CASE("orchestrator update rqp package no-ops for local source", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-rqp-update-local-noop"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "local-only-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-local"))
    );
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "local-only-artifact" / "local-only-artifact@1.0.0-1+r0";

    (void)run_reqpack(tempDir.path(), configPath, {"install", localPackage.string()});
    REQUIRE(std::filesystem::exists(stateDir / "installed.txt"));

    const std::string output = run_reqpack(tempDir.path(), configPath, {"update", "rqp", "local-only-artifact"});

    CHECK((output.find("0 ok") != std::string::npos || output.find("0 fail") != std::string::npos));
    CHECK(read_file(stateDir / "installed.txt") == "hello-local");
}

TEST_CASE("orchestrator update rqp package installs newer repository version", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-rqp-update-repo"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path artifactV1 = build_rqp_package(
        tempDir.path() / "v1",
        "updatable-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\ncontext.artifacts.register_file(out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-v1"))
    );
    const std::filesystem::path artifactV2 = build_rqp_package(
        tempDir.path() / "v2",
        "updatable-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\ncontext.artifacts.register_file(out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-v2")),
        std::nullopt,
        "1.1.0"
    );
    const std::filesystem::path indexPath = tempDir.path() / "index.json";
    write_file(indexPath,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"packages\": [\n"
        "    {\n"
        "      \"name\": \"updatable-artifact\",\n"
        "      \"version\": \"1.0.0\",\n"
        "      \"release\": 1,\n"
        "      \"revision\": 0,\n"
        "      \"architecture\": \"noarch\",\n"
        "      \"summary\": \"repo package\",\n"
        "      \"url\": \"file://" + artifactV1.string() + "\",\n"
        "      \"packageSha256\": \"" + sha256_file_hex(artifactV1) + "\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"updatable-artifact\",\n"
        "      \"version\": \"1.1.0\",\n"
        "      \"release\": 1,\n"
        "      \"revision\": 0,\n"
        "      \"architecture\": \"noarch\",\n"
        "      \"summary\": \"repo package\",\n"
        "      \"url\": \"file://" + artifactV2.string() + "\",\n"
        "      \"packageSha256\": \"" + sha256_file_hex(artifactV2) + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n");
    const std::filesystem::path configPath = write_config_with_rq_repositories(
        tempDir.path(),
        pluginDirectory,
        {"file://" + indexPath.string()}
    );
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "updatable-artifact" / "updatable-artifact@1.1.0-1+r0";

    (void)run_reqpack(tempDir.path(), configPath, {"install", "rqp", "updatable-artifact@1.0.0"});
    const std::string output = run_reqpack(tempDir.path(), configPath, {"update", "rqp", "updatable-artifact"});
    CHECK(output.find("UPDATE: rqp:updatable-artifact") != std::string::npos);
    CHECK(std::filesystem::exists(stateDir / "installed.txt"));
    CHECK(read_file(stateDir / "installed.txt") == "hello-v2");
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

TEST_CASE("orchestrator audit command exports sarif without executing plugin install", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-audit-export"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    const std::filesystem::path outputPath = tempDir.path() / "audit.sarif";

    write_file(feedPath, R"([
        {
            "id": "CVE-2026-demo",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "critical demo package issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "demo-osv", "name": "sample"},
                "versions": ["1.0.0"]
            }]
        }
    ])");
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  security = {\n"
        "    osvFeedUrl = '" + feedPath.string() + "',\n"
        "    osvDatabasePath = '" + (tempDir.path() / "osv-db").string() + "',\n"
        "    osvRefreshMode = 'always',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "audit",
        "apply",
        "sample@1.0.0",
        "--output",
        outputPath.string(),
    }, status);

    CHECK(status == 0);
    CHECK(output.find(outputPath.string()) != std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string report = read_file(outputPath);
    CHECK(report.find("\"runs\"") != std::string::npos);
    CHECK(report.find("\"ruleId\": \"CVE-2026-demo\"") != std::string::npos);

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    CHECK_FALSE(std::filesystem::exists(installMarker));
}

TEST_CASE("orchestrator audit command returns non-zero on stdout findings", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-audit-stdout"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";

    write_file(feedPath, R"([
        {
            "id": "CVE-2026-demo",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "critical demo package issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "demo-osv", "name": "sample"},
                "versions": ["1.0.0"]
            }]
        }
    ])");
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  security = {\n"
        "    osvFeedUrl = '" + feedPath.string() + "',\n"
        "    osvDatabasePath = '" + (tempDir.path() / "osv-db").string() + "',\n"
        "    osvRefreshMode = 'always',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "audit",
        "apply",
        "sample@1.0.0",
    }, status);

    CHECK(status != 0);
    CHECK(output.find("CVE-2026-demo") != std::string::npos);
    CHECK(output.find("react") == std::string::npos);

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    CHECK_FALSE(std::filesystem::exists(installMarker));
}

TEST_CASE("orchestrator audit system-only request audits installed packages", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-audit-system-only"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";

    write_file(feedPath, R"([
        {
            "id": "CVE-2026-system-only",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "installed package issue",
            "severity": [{"type": "CVSS_V3", "score": "7.5"}],
            "affected": [{
                "package": {"ecosystem": "demo-osv", "name": "apply"},
                "versions": ["1.0.0"]
            }]
        }
    ])");
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  security = {\n"
        "    osvFeedUrl = '" + feedPath.string() + "',\n"
        "    osvDatabasePath = '" + (tempDir.path() / "osv-db").string() + "',\n"
        "    osvRefreshMode = 'always',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "audit",
        "apply",
    }, status);

    CHECK(status != 0);
    CHECK(output.find("CVE-2026-system-only") != std::string::npos);
    CHECK(output.find("apply\tapply\t1.0.0") != std::string::npos);

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    CHECK_FALSE(std::filesystem::exists(installMarker));
}

TEST_CASE("reqpack install stdin batches install commands until eof", "[integration][orchestrator][stdin]") {
    TempDir tempDir{"reqpack-orchestrator-install-stdin"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}

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
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages)
  local path = context.plugin.dir .. "/state/install.txt"
  context.exec.run("mkdir -p '" .. context.plugin.dir .. "/state'")
  for _, package in ipairs(packages) do
    context.exec.run("printf '%s\\n' '" .. package.name .. "' >> '" .. path .. "'")
  end
  return true
end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.outdated(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return {} end
function plugin.shutdown() return true end
)");

    const std::string output = run_reqpack_with_stdin(
        tempDir.path(),
        configPath,
        {"install", "--stdin"},
        "install apply alpha\ninstall apply beta\n"
    );
    (void)output;

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(installMarker));
    const std::string installed = read_file(installMarker);
    CHECK(installed.find("alpha\n") != std::string::npos);
    CHECK(installed.find("beta\n") != std::string::npos);
}

TEST_CASE("reqpack serve stdin executes commands line by line and continues after parse errors", "[integration][orchestrator][stdin]") {
    TempDir tempDir{"reqpack-orchestrator-serve-stdin"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack_with_stdin(
        tempDir.path(),
        configPath,
        {"serve", "--stdin"},
        "install apply alpha\ninstall \"broken\nlist apply\n"
    );

    const std::filesystem::path installMarker = pluginDirectory / "apply" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(installMarker));
    CHECK(read_file(installMarker) == "alpha");
    CHECK(output.find("stdin line 2: invalid command syntax") != std::string::npos);
    CHECK(output.find("[apply] (list) apply 1.0.0 - listed from " + (pluginDirectory / "apply").string()) != std::string::npos);
}

TEST_CASE("reqpack install returns non-zero when security validation blocks execution", "[integration][orchestrator][security]") {
    TempDir tempDir{"reqpack-orchestrator-security-block"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";

    write_file(feedPath, R"([
        {
            "id": "CVE-2026-demo",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "critical demo package issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "demo-osv", "name": "sample"},
                "versions": ["1.0.0"]
            }]
        }
    ])");
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  security = {\n"
        "    osvFeedUrl = '" + feedPath.string() + "',\n"
        "    osvDatabasePath = '" + (tempDir.path() / "osv-db").string() + "',\n"
        "    osvRefreshMode = 'always',\n"
        "    severityThreshold = 'critical',\n"
        "    onUnsafe = 'abort',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {"install", "apply", "sample@1.0.0"},
        status
    );

    REQUIRE(status != 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(output.find("execution blocked by security policy") != std::string::npos);
    CHECK(output.find("critical demo package issue") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pluginDirectory / "apply" / "state" / "install.txt"));
}

TEST_CASE("reqpack serve remote text mode supports token auth", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-text"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "auth token secret\n"));
    const auto authResponse = read_text_protocol_response(client);
    CHECK(authResponse.first == "OK");
    CHECK(authResponse.second.empty());

    REQUIRE(send_socket_text(client, "list apply\n"));
    const auto listResponse = read_text_protocol_response(client);
    CHECK(listResponse.first == "OK");
    CHECK(listResponse.second.find("[apply] (list) apply 1.0.0 - listed from " + (pluginDirectory / "apply").string()) != std::string::npos);
    ::close(client);
}

TEST_CASE("reqpack serve remote returns error when security validation blocks execution", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-security-block"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path feedPath = tempDir.path() / "osv-feed.json";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    write_file(feedPath, R"([
        {
            "id": "CVE-2026-demo",
            "modified": "2026-01-01T00:00:00Z",
            "summary": "critical demo package issue",
            "severity": [{"type": "CVSS_V3", "score": "9.8"}],
            "affected": [{
                "package": {"ecosystem": "demo-osv", "name": "sample"},
                "versions": ["1.0.0"]
            }]
        }
    ])");
    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  security = {\n"
        "    osvFeedUrl = '" + feedPath.string() + "',\n"
        "    osvDatabasePath = '" + (tempDir.path() / "osv-db").string() + "',\n"
        "    osvRefreshMode = 'always',\n"
        "    severityThreshold = 'critical',\n"
        "    onUnsafe = 'abort',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "auth token secret\n"));
    const auto authResponse = read_text_protocol_response(client);
    CHECK(authResponse.first == "OK");

    REQUIRE(send_socket_text(client, "install apply sample@1.0.0\n"));
    const auto installResponse = read_text_protocol_response(client);
    CHECK(installResponse.first == "ERR");
    CHECK(installResponse.second.find("execution blocked by security policy") != std::string::npos);
    CHECK(installResponse.second.find("critical demo package issue") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pluginDirectory / "apply" / "state" / "install.txt"));
    ::close(client);
}

TEST_CASE("reqpack serve remote readonly rejects mutating commands", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-readonly"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--readonly"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "install apply alpha\n"));
    const auto response = read_text_protocol_response(client);
    CHECK(response.first == "ERR");
    CHECK(response.second.find("readonly") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pluginDirectory / "apply" / "state" / "install.txt"));
    ::close(client);
}

TEST_CASE("reqpack serve remote json mode returns json responses", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-json"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--json", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "{\"token\":\"secret\",\"command\":\"list apply\"}\n"));
    const std::string response = read_json_response_line(client);
    CHECK(response.find("\"ok\":true") != std::string::npos);
    CHECK(response.find("listed from") != std::string::npos);
    ::close(client);
}

TEST_CASE("reqpack serve remote autodetects json clients without json flag", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-auto-json"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "{\"token\":\"secret\",\"command\":\"list apply\"}\n"));
    const std::string response = read_json_response_line(client);
    CHECK(response.find("\"ok\":true") != std::string::npos);
    CHECK(response.find("listed from") != std::string::npos);
    ::close(client);
}

TEST_CASE("reqpack serve remote enforces max connections", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-limit"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--max-connections", "1"
    }, logPath);

    const int firstClient = connect_with_retry("127.0.0.1", port);
    const int secondClient = connect_with_retry("127.0.0.1", port);
    const auto response = read_text_protocol_response(secondClient);
    CHECK(response.first == "ERR");
    CHECK(response.second.find("max connections") != std::string::npos);
    ::close(firstClient);
    ::close(secondClient);
}

TEST_CASE("reqpack serve remote supports admin commands from server remote users", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-admin"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_users(tempDir.path(),
        "return {\n"
        "  users = {\n"
        "    alice = { token = 'user-token' },\n"
        "    root = { token = 'admin-token', isAdmin = true },\n"
        "  },\n"
        "}\n");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port)
    }, logPath);

    const int userClient = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(userClient, "auth token user-token\n"));
    const auto userAuth = read_text_protocol_response(userClient);
    CHECK(userAuth.first == "OK");

    REQUIRE(send_socket_text(userClient, "connections count\n"));
    const auto forbidden = read_text_protocol_response(userClient);
    CHECK(forbidden.first == "ERR");
    CHECK(forbidden.second.find("admin privileges") != std::string::npos);

    const int adminClient = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(adminClient, "auth token admin-token\n"));
    const auto adminAuth = read_text_protocol_response(adminClient);
    CHECK(adminAuth.first == "OK");

    REQUIRE(send_socket_text(adminClient, "connections count\n"));
    const auto countResponse = read_text_protocol_response(adminClient);
    CHECK(countResponse.first == "OK");
    CHECK(countResponse.second == "2");

    REQUIRE(send_socket_text(adminClient, "connections list\n"));
    const auto listResponse = read_text_protocol_response(adminClient);
    CHECK(listResponse.first == "OK");
    CHECK(listResponse.second.find("user=alice") != std::string::npos);
    CHECK(listResponse.second.find("user=root") != std::string::npos);
    CHECK(listResponse.second.find("admin=true") != std::string::npos);

    REQUIRE(send_socket_text(userClient, "exit\n"));
    const auto exitResponse = read_text_protocol_response(userClient);
    CHECK(exitResponse.first == "OK");

    REQUIRE(send_socket_text(adminClient, "shutdown\n"));
    const auto shutdownResponse = read_text_protocol_response(adminClient);
    CHECK(shutdownResponse.first == "OK");
    CHECK(shutdownResponse.second.find("shutting down") != std::string::npos);

    ::close(userClient);
    ::close(adminClient);
}

TEST_CASE("reqpack serve remote reload-config reloads users and readonly state", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-reload"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_users(tempDir.path(),
        "return {\n"
        "  users = {\n"
        "    root = { token = 'admin-token', isAdmin = true },\n"
        "  },\n"
        "}\n");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port)
    }, logPath);

    const int adminClient = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(adminClient, "auth token admin-token\n"));
    const auto adminAuth = read_text_protocol_response(adminClient);
    CHECK(adminAuth.first == "OK");

    write_file(configPath,
        "return {\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (tempDir.path() / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "    shutDownPluginsOnExit = true,\n"
        "  },\n"
        "  interaction = { interactive = false },\n"
        "  remote = { readonly = true },\n"
        "}\n");
    write_remote_users(tempDir.path(),
        "return {\n"
        "  users = {\n"
        "    root = { token = 'new-admin-token', isAdmin = true },\n"
        "  },\n"
        "}\n");

    REQUIRE(send_socket_text(adminClient, "reload-config\n"));
    const auto reloadResponse = read_text_protocol_response(adminClient);
    CHECK(reloadResponse.first == "OK");

    const int staleClient = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(staleClient, "auth token admin-token\n"));
    const auto staleAuth = read_text_protocol_response(staleClient);
    CHECK(staleAuth.first == "ERR");
    ::close(staleClient);

    const int newAdminClient = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(newAdminClient, "auth token new-admin-token\n"));
    const auto newAuth = read_text_protocol_response(newAdminClient);
    CHECK(newAuth.first == "OK");

    REQUIRE(send_socket_text(newAdminClient, "install apply alpha\n"));
    const auto readonlyResponse = read_text_protocol_response(newAdminClient);
    CHECK(readonlyResponse.first == "ERR");
    CHECK(readonlyResponse.second.find("readonly") != std::string::npos);

    REQUIRE(send_socket_text(newAdminClient, "shutdown\n"));
    const auto shutdownResponse = read_text_protocol_response(newAdminClient);
    CHECK(shutdownResponse.first == "OK");

    ::close(adminClient);
    ::close(newAdminClient);
}

TEST_CASE("reqpack remote loads profile from default remote.lua and forwards command", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-profile"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    url = 'tcp://127.0.0.1:" + std::to_string(port) + "',\n"
        "    token = 'secret',\n"
        "  },\n"
        "}\n");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int warmupClient = connect_with_retry("127.0.0.1", port);
    ::close(warmupClient);

    const std::string output = run_reqpack_with_home(tempDir.path(), configPath, tempDir.path(), {"remote", "dev", "list", "apply"});
    CHECK(output.find("[apply] (list) apply 1.0.0 - listed from " + (pluginDirectory / "apply").string()) != std::string::npos);
}

TEST_CASE("reqpack remote preserves forwarded command flags after profile name", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-profile-flags"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    host = '127.0.0.1',\n"
        "    port = " + std::to_string(port) + ",\n"
        "  },\n"
        "}\n");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port)
    }, logPath);

    const int warmupClient = connect_with_retry("127.0.0.1", port);
    ::close(warmupClient);

    const std::string output = run_reqpack_with_home(tempDir.path(), configPath, tempDir.path(), {
        "remote", "dev", "sbom", "apply", "sample", "--sbom-format", "json"
    });
    CHECK(output.find("\"packages\"") != std::string::npos);
    CHECK(output.find("\"name\": \"sample\"") != std::string::npos);
}

TEST_CASE("reqpack remote uploads local install file over text protocol", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-upload"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    url = 'tcp://127.0.0.1:" + std::to_string(port) + "',\n"
        "    token = 'secret',\n"
        "  },\n"
        "}\n");

    const std::filesystem::path uploadPath = tempDir.path() / "sample.pkg";
    write_file(uploadPath, "payload-content");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int warmupClient = connect_with_retry("127.0.0.1", port);
    ::close(warmupClient);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "remote", "dev", "install", "apply", uploadPath.string()
    }, status);
    INFO(output);
    CHECK(status == 0);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "local.txt").find("sample.pkg") != std::string::npos);
}

TEST_CASE("reqpack remote rejects file upload over json protocol", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-upload-json"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    host = '127.0.0.1',\n"
        "    port = " + std::to_string(port) + ",\n"
        "    protocol = 'json',\n"
        "    token = 'secret',\n"
        "  },\n"
        "}\n");

    const std::filesystem::path uploadPath = tempDir.path() / "sample.pkg";
    write_file(uploadPath, "payload-content");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret", "--json"
    }, logPath);

    const int warmupClient = connect_with_retry("127.0.0.1", port);
    ::close(warmupClient);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "remote", "dev", "install", "apply", uploadPath.string()
    }, status);
    CHECK(status != 0);
    CHECK(output.find("file upload requires text protocol") != std::string::npos);
}

TEST_CASE("reqpack remote upload respects readonly server mode", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-upload-readonly"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);
    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    url = 'tcp://127.0.0.1:" + std::to_string(port) + "',\n"
        "    token = 'secret',\n"
        "  },\n"
        "}\n");

    const std::filesystem::path uploadPath = tempDir.path() / "sample.pkg";
    write_file(uploadPath, "payload-content");

    ServerProcess server(tempDir.path(), configPath, tempDir.path(), {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret", "--readonly"
    }, logPath);

    const int warmupClient = connect_with_retry("127.0.0.1", port);
    ::close(warmupClient);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "remote", "dev", "install", "apply", uploadPath.string()
    }, status);
    CHECK(status != 0);
    CHECK(output.find("readonly") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pluginDirectory / "apply" / "state" / "local.txt"));
}

TEST_CASE("orchestrator sys plugin maps logical packages to apt backend", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-sys-apt"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path aptLog = tempDir.path() / "apt.log";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "sys");

    write_file(fakeBin / "apt-get",
        "#!/bin/sh\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(aptLog.string()) + "\n"
        "exit 0\n");
    write_file(fakeBin / "dpkg-query",
        "#!/bin/sh\n"
        "exit 1\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "apt-get").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dpkg-query").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_SYS_BACKEND", "apt"},
            {"REQPACK_SYS_NO_SUDO", "1"},
            {"REQPACK_SYS_NIX_BIN", (fakeBin / "missing-nix-env").string()},
            {"REQPACK_SYS_APT_BIN", (fakeBin / "apt-get").string()},
            {"REQPACK_SYS_DPKG_QUERY_BIN", (fakeBin / "dpkg-query").string()},
        },
        {"install", "sys", "java", "maven"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    CHECK(output.find("INSTALL done:  2 ok,  0 skipped,  0 failed") != std::string::npos);
    const std::string log = read_file(aptLog);
    CHECK(log.find("update") != std::string::npos);
    CHECK(log.find("install -y") != std::string::npos);
    CHECK(log.find("default-jdk") != std::string::npos);
    CHECK(log.find("maven") != std::string::npos);
}

TEST_CASE("orchestrator install maven provisions sys requirements before invoking mvn", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-sys-maven"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path aptLog = tempDir.path() / "apt.log";
    const std::filesystem::path mvnLog = tempDir.path() / "mvn.log";
    const std::filesystem::path provisionedMarker = tempDir.path() / "provisioned.marker";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "sys");
    copy_repo_plugin(pluginDirectory, "maven");

    write_file(fakeBin / "java",
        "#!/bin/sh\n"
        "exit 0\n");
    write_file(fakeBin / "javac",
        "#!/bin/sh\n"
        "exit 0\n");
    write_file(fakeBin / "mvn",
        "#!/bin/sh\n"
        "[ -f " + escape_shell_arg(provisionedMarker.string()) + " ] || exit 1\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(mvnLog.string()) + "\n"
        "exit 0\n");
    write_file(fakeBin / "dpkg-query",
        "#!/bin/sh\n"
        "exit 1\n");

    write_file(fakeBin / "apt-get",
        "#!/bin/sh\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(aptLog.string()) + "\n"
        ": > " + escape_shell_arg(provisionedMarker.string()) + "\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "apt-get").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "java").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "javac").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "mvn").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dpkg-query").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_SYS_BACKEND", "apt"},
            {"REQPACK_SYS_NO_SUDO", "1"},
            {"REQPACK_SYS_NIX_BIN", (fakeBin / "missing-nix-env").string()},
            {"REQPACK_SYS_APT_BIN", (fakeBin / "apt-get").string()},
            {"REQPACK_SYS_DPKG_QUERY_BIN", (fakeBin / "dpkg-query").string()},
        },
        {"install", "maven", "org.junit:junit:4.13"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    CHECK(output.find("INSTALL done:  3 ok,  0 skipped,  0 failed") != std::string::npos);
    const std::string aptInstallLog = read_file(aptLog);
    CHECK(aptInstallLog.find("install -y") != std::string::npos);
    CHECK(aptInstallLog.find("default-jdk") != std::string::npos);
    CHECK(aptInstallLog.find("maven") != std::string::npos);
    CHECK(std::filesystem::exists(provisionedMarker));
    const std::string mavenInstallLog = read_file(mvnLog);
    CHECK(mavenInstallLog.find("dependency:get") != std::string::npos);
    CHECK(mavenInstallLog.find("org.junit:junit:4.13") != std::string::npos);
}

TEST_CASE("orchestrator sys plugin installs packages via nix backend", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-sys-nix"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path nixLog = tempDir.path() / "nix.log";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "sys");

    write_file(fakeBin / "nix-env",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-q\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(nixLog.string()) + "\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "nix-env").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_SYS_BACKEND", "nix"},
            {"REQPACK_SYS_NIX_BIN", (fakeBin / "nix-env").string()},
        },
        {"install", "sys", "alpha-tool", "beta-tool"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    CHECK(output.find("INSTALL done:  2 ok,  0 skipped,  0 failed") != std::string::npos);
    const std::string log = read_file(nixLog);
    CHECK(log.find("-iA") != std::string::npos);
    CHECK(log.find("nixpkgs.alpha-tool") != std::string::npos);
    CHECK(log.find("nixpkgs.beta-tool") != std::string::npos);
}

TEST_CASE("orchestrator sys plugin falls back to nix when backend package is unavailable", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-sys-nix-fallback"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path aptLog = tempDir.path() / "apt.log";
    const std::filesystem::path aptCacheLog = tempDir.path() / "apt-cache.log";
    const std::filesystem::path nixLog = tempDir.path() / "nix.log";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "sys");

    write_file(fakeBin / "apt-get",
        "#!/bin/sh\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(aptLog.string()) + "\n"
        "exit 0\n");
    write_file(fakeBin / "apt-cache",
        "#!/bin/sh\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(aptCacheLog.string()) + "\n"
        "if [ \"$1\" = \"show\" ] && [ \"$2\" = \"fallbackpkg\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "exit 0\n");
    write_file(fakeBin / "dpkg-query",
        "#!/bin/sh\n"
        "exit 1\n");
    write_file(fakeBin / "nix-env",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-q\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "if [ \"$1\" = \"-qaA\" ] && [ \"$2\" = \"nixpkgs.fallbackpkg\" ]; then\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(nixLog.string()) + "\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "apt-get").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "apt-cache").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dpkg-query").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "nix-env").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_SYS_BACKEND", "apt"},
            {"REQPACK_SYS_NO_SUDO", "1"},
            {"REQPACK_SYS_APT_BIN", (fakeBin / "apt-get").string()},
            {"REQPACK_SYS_APT_CACHE_BIN", (fakeBin / "apt-cache").string()},
            {"REQPACK_SYS_DPKG_QUERY_BIN", (fakeBin / "dpkg-query").string()},
            {"REQPACK_SYS_NIX_BIN", (fakeBin / "nix-env").string()},
        },
        {"install", "sys", "fallbackpkg"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    CHECK(output.find("INSTALL done:  1 ok,  0 skipped,  0 failed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(aptLog));
    const std::string aptCacheOutput = read_file(aptCacheLog);
    CHECK(aptCacheOutput.find("show fallbackpkg") != std::string::npos);
    const std::string nixOutput = read_file(nixLog);
    CHECK(nixOutput.find("-iA") != std::string::npos);
    CHECK(nixOutput.find("nixpkgs.fallbackpkg") != std::string::npos);
}

TEST_CASE("orchestrator sys plugin bootstraps nix when nix backend is selected but missing", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-sys-nix-bootstrap"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path nixLog = tempDir.path() / "nix.log";
    const std::filesystem::path bootstrapLog = tempDir.path() / "bootstrap.log";
    const std::filesystem::path installedNix = tempDir.path() / "home" / ".nix-profile" / "bin" / "nix-env";
    const std::filesystem::path bootstrapScript = fakeBin / "bootstrap-nix";
    std::filesystem::create_directories(fakeBin);
    std::filesystem::create_directories(installedNix.parent_path());

    copy_repo_plugin(pluginDirectory, "sys");

    write_file(bootstrapScript,
        "#!/bin/sh\n"
        "printf '%s\\n' bootstrap >> " + escape_shell_arg(bootstrapLog.string()) + "\n"
        "mkdir -p " + escape_shell_arg(installedNix.parent_path().string()) + "\n"
        "cat > " + escape_shell_arg(installedNix.string()) + " <<'EOF'\n"
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-q\" ]; then\n"
        "  exit 1\n"
        "fi\n"
        "printf '%s\\n' \"$*\" >> " + escape_shell_arg(nixLog.string()) + "\n"
        "exit 0\n"
        "EOF\n"
        "chmod +x " + escape_shell_arg(installedNix.string()) + "\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg(bootstrapScript.string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");
    const std::filesystem::path homePath = tempDir.path() / "home";
    std::filesystem::create_directories(homePath);

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        homePath,
        {
            {"PATH", pathValue},
            {"REQPACK_SYS_BACKEND", "nix"},
            {"REQPACK_SYS_NIX_INSTALL_CMD", bootstrapScript.string()},
        },
        {"install", "sys", "bootstrap-tool"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    CHECK(output.find("INSTALL done:  1 ok,  0 skipped,  0 failed") != std::string::npos);
    CHECK(std::filesystem::exists(installedNix));
    CHECK(read_file(bootstrapLog).find("bootstrap") != std::string::npos);
    const std::string nixOutput = read_file(nixLog);
    CHECK(nixOutput.find("-iA") != std::string::npos);
    CHECK(nixOutput.find("nixpkgs.bootstrap-tool") != std::string::npos);
}

TEST_CASE("orchestrator sys plugin delegates install to additional linux backends", "[integration][orchestrator][service][sys]") {
    struct BackendCase {
        std::string backend;
        std::string binaryEnv;
        std::string binaryName;
        std::string queryEnv;
        std::string queryName;
        std::string installNeedle;
        std::string logicalNeedle;
    };

    const std::vector<BackendCase> cases{
        {"yum", "REQPACK_SYS_YUM_BIN", "yum", "REQPACK_SYS_RPM_BIN", "rpm", "install -y", "maven"},
        {"zypper", "REQPACK_SYS_ZYPPER_BIN", "zypper", "REQPACK_SYS_RPM_BIN", "rpm", "install --auto-agree-with-licenses", "maven"},
        {"pacman", "REQPACK_SYS_PACMAN_BIN", "pacman", "REQPACK_SYS_PACMAN_BIN", "pacman", "-S --noconfirm --needed", "maven"},
        {"apk", "REQPACK_SYS_APK_BIN", "apk", "REQPACK_SYS_APK_BIN", "apk", "add", "maven"},
        {"xbps", "REQPACK_SYS_XBPS_INSTALL_BIN", "xbps-install", "REQPACK_SYS_XBPS_QUERY_BIN", "xbps-query", "-Sy", "maven"},
        {"eopkg", "REQPACK_SYS_EOPKG_BIN", "eopkg", "REQPACK_SYS_EOPKG_BIN", "eopkg", "install -y", "maven"},
        {"urpmi", "REQPACK_SYS_URPMI_BIN", "urpmi", "REQPACK_SYS_RPM_BIN", "rpm", "--auto", "maven"},
        {"emerge", "REQPACK_SYS_EMERGE_BIN", "emerge", "REQPACK_SYS_EQUERY_BIN", "equery", "--ask=n", "dev-java/maven-bin"},
    };

    for (const BackendCase& testCase : cases) {
        CAPTURE(testCase.backend);
        TempDir tempDir{"reqpack-orchestrator-sys-" + testCase.backend};
        const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
        const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
        const std::filesystem::path fakeBin = tempDir.path() / "bin";
        const std::filesystem::path backendLog = tempDir.path() / (testCase.backend + ".log");
        std::filesystem::create_directories(fakeBin);

        copy_repo_plugin(pluginDirectory, "sys");

        write_file(fakeBin / testCase.binaryName,
            "#!/bin/sh\n"
            "printf '%s\n' \"$*\" >> " + escape_shell_arg(backendLog.string()) + "\n"
            "exit 0\n");
        REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / testCase.binaryName).string())).c_str()) == 0);

        std::vector<std::pair<std::string, std::string>> environment{
            {"REQPACK_SYS_BACKEND", testCase.backend},
            {"REQPACK_SYS_NO_SUDO", "1"},
            {"REQPACK_SYS_NIX_BIN", (fakeBin / "missing-nix-env").string()},
            {testCase.binaryEnv, (fakeBin / testCase.binaryName).string()},
        };

        if (!testCase.queryEnv.empty()) {
            std::string queryScript =
                "#!/bin/sh\n"
                "exit 1\n";
            if (testCase.backend == "pacman") {
                queryScript =
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"-Q\" ]; then exit 1; fi\n"
                    "printf '%s\n' \"$*\" >> " + escape_shell_arg(backendLog.string()) + "\n"
                    "exit 0\n";
            }
            else if (testCase.backend == "apk") {
                queryScript =
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"info\" ] && [ \"$2\" = \"-e\" ]; then exit 1; fi\n"
                    "printf '%s\n' \"$*\" >> " + escape_shell_arg(backendLog.string()) + "\n"
                    "exit 0\n";
            }
            else if (testCase.backend == "eopkg") {
                queryScript =
                    "#!/bin/sh\n"
                    "if [ \"$1\" = \"list-installed\" ]; then exit 1; fi\n"
                    "printf '%s\n' \"$*\" >> " + escape_shell_arg(backendLog.string()) + "\n"
                    "exit 0\n";
            }
            write_file(fakeBin / testCase.queryName, queryScript);
            REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / testCase.queryName).string())).c_str()) == 0);
            environment.push_back({testCase.queryEnv, (fakeBin / testCase.queryName).string()});
        }

        if (testCase.backend == "emerge") {
            write_file(fakeBin / "equery",
                "#!/bin/sh\n"
                "exit 1\n");
            REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "equery").string())).c_str()) == 0);
            environment.push_back({"REQPACK_SYS_EQUERY_BIN", (fakeBin / "equery").string()});
        }

        const char* currentPath = std::getenv("PATH");
        const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");
        environment.push_back({"PATH", pathValue});

        int status = 0;
        const std::string output = run_reqpack_with_home_env_and_status(
            tempDir.path(),
            configPath,
            tempDir.path(),
            environment,
            {"install", "sys", "maven"},
            status
        );

        CHECK(status == 0);
        CHECK(output.find("[error]") == std::string::npos);
        CHECK(output.find("INSTALL done:  1 ok,  0 skipped,  0 failed") != std::string::npos);
        const std::string log = read_file(backendLog);
        CHECK(log.find(testCase.installNeedle) != std::string::npos);
        CHECK(log.find(testCase.logicalNeedle) != std::string::npos);
    }
}
