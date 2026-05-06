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

#include "core/host_info.h"
#include "core/registry.h"
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
        "  security = {\n"
        "    autoFetch = false,\n"
        "  },\n"
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

std::filesystem::path write_config_with_proxy(
    const std::filesystem::path& root,
    const std::filesystem::path& pluginDirectory,
    const std::string& defaultTarget,
    const std::string& targetsLua
) {
    const std::filesystem::path configPath = root / "config.lua";
    write_file(configPath,
        "return {\n"
        "  security = {\n"
        "    autoFetch = false,\n"
        "  },\n"
        "  execution = {\n"
        "    useTransactionDb = false,\n"
        "    deleteCommittedTransactions = false,\n"
        "    checkVirtualFileSystemWrite = false,\n"
        "    transactionDatabasePath = '" + (root / "transactions").string() + "',\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "    proxies = {\n"
        "      java = {\n"
        "        default = '" + defaultTarget + "',\n"
        "        targets = " + targetsLua + ",\n"
        "      },\n"
        "    },\n"
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
        "  security = {\n"
        "    autoFetch = false,\n"
        "  },\n"
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

std::filesystem::path write_config_with_maven_repositories(
    const std::filesystem::path& root,
    const std::filesystem::path& pluginDirectory,
    const std::string& repositoriesLua
) {
    const std::filesystem::path configPath = root / "config.lua";
    write_file(configPath,
        "return {\n"
        "  security = {\n"
        "    autoFetch = false,\n"
        "  },\n"
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
        "  repositories = {\n"
        "    maven = {\n" + repositoriesLua +
        "    },\n"
        "  },\n"
        "  rqp = {\n"
        "    statePath = '" + (root / "rqp-state").string() + "',\n"
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
        std::string hash = overrideHash.value_or("");
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

std::filesystem::path build_zip_archive(
    const std::filesystem::path& root,
    const std::string& archiveName,
    const std::vector<std::pair<std::string, std::string>>& files
) {
    const std::filesystem::path sourceRoot = root / (archiveName + "-src");
    std::filesystem::create_directories(sourceRoot);
    for (const auto& [relativePath, content] : files) {
        write_file(sourceRoot / relativePath, content);
    }

    const std::filesystem::path archivePath = root / archiveName;
    const std::string zipCommand = "cd " + escape_shell_arg(sourceRoot.string()) + " && zip -qr " + escape_shell_arg(archivePath.string()) + " .";
    REQUIRE(std::system(zipCommand.c_str()) == 0);
    return archivePath;
}

std::filesystem::path build_encrypted_zip_archive(
    const std::filesystem::path& root,
    const std::string& archiveName,
    const std::string& password,
    const std::vector<std::pair<std::string, std::string>>& files
) {
    const std::filesystem::path sourceRoot = root / (archiveName + "-src");
    std::filesystem::create_directories(sourceRoot);
    for (const auto& [relativePath, content] : files) {
        write_file(sourceRoot / relativePath, content);
    }

    const std::filesystem::path archivePath = root / archiveName;
    const std::string zipCommand = "cd " + escape_shell_arg(sourceRoot.string()) + " && zip -q -r -P " +
        escape_shell_arg(password) + " " + escape_shell_arg(archivePath.string()) + " .";
    REQUIRE(std::system(zipCommand.c_str()) == 0);
    return archivePath;
}

std::filesystem::path build_encrypted_seven_zip_archive(
    const std::filesystem::path& root,
    const std::string& archiveName,
    const std::string& password,
    const std::vector<std::pair<std::string, std::string>>& files
) {
    const std::filesystem::path sourceRoot = root / (archiveName + "-src");
    std::filesystem::create_directories(sourceRoot);
    for (const auto& [relativePath, content] : files) {
        write_file(sourceRoot / relativePath, content);
    }

    const std::filesystem::path archivePath = root / archiveName;
    const std::string sevenZipCommand = "cd " + escape_shell_arg(sourceRoot.string()) + " && 7z a -y -p" +
        escape_shell_arg(password) + " -mhe=on " + escape_shell_arg(archivePath.string()) + " .";
    REQUIRE(std::system(sevenZipCommand.c_str()) == 0);
    return archivePath;
}

std::filesystem::path build_tar_gz_archive(
    const std::filesystem::path& root,
    const std::string& archiveName,
    const std::vector<std::pair<std::string, std::string>>& files
) {
    const std::filesystem::path sourceRoot = root / (archiveName + "-src");
    std::filesystem::create_directories(sourceRoot);
    for (const auto& [relativePath, content] : files) {
        write_file(sourceRoot / relativePath, content);
    }

    const std::filesystem::path archivePath = root / archiveName;
    const std::string tarCommand = "tar -C " + escape_shell_arg(sourceRoot.string()) + " -czf " + escape_shell_arg(archivePath.string()) + " .";
    REQUIRE(std::system(tarCommand.c_str()) == 0);
    return archivePath;
}

std::filesystem::path wrap_archive_with_gpg(
    const std::filesystem::path& root,
    const std::filesystem::path& sourcePath,
    const std::string& wrapperName,
    const std::string& password
) {
    const std::filesystem::path wrapperPath = root / wrapperName;
    const std::string gpgCommand = "gpg --batch --yes --pinentry-mode loopback --passphrase " + escape_shell_arg(password) +
        " -o " + escape_shell_arg(wrapperPath.string()) + " -c " + escape_shell_arg(sourcePath.string());
    REQUIRE(std::system(gpgCommand.c_str()) == 0);
    return wrapperPath;
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
    const std::filesystem::path reqpackConfig = root / ".config" / "reqpack";
    std::filesystem::create_directories(reqpackConfig);
    const std::filesystem::path profilePath = reqpackConfig / "remote.lua";
    write_file(profilePath, content);
    return profilePath;
}

std::filesystem::path write_remote_users(const std::filesystem::path& root, const std::string& content) {
    return write_remote_profiles(root, content);
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

std::string run_reqpack_with_home(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::string>& arguments
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && env HOME=" + escape_shell_arg(homePath.string()) +
        " XDG_CONFIG_HOME=" + escape_shell_arg((homePath / ".config").string()) +
        " XDG_DATA_HOME=" + escape_shell_arg((homePath / ".local" / "share").string()) +
        " XDG_CACHE_HOME=" + escape_shell_arg((homePath / ".cache").string()) +
        " " + escape_shell_arg((build_root() / "rqp").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

std::string run_reqpack_with_home_and_env(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::pair<std::string, std::string>>& environment,
    const std::vector<std::string>& arguments
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && env HOME=" + escape_shell_arg(homePath.string()) +
        " XDG_CONFIG_HOME=" + escape_shell_arg((homePath / ".config").string()) +
        " XDG_DATA_HOME=" + escape_shell_arg((homePath / ".local" / "share").string()) +
        " XDG_CACHE_HOME=" + escape_shell_arg((homePath / ".cache").string());
    for (const auto& [key, value] : environment) {
        command += " " + key + "=" + escape_shell_arg(value);
    }
    command += " " + escape_shell_arg((build_root() / "rqp").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

void require_command_success(const std::string& command) {
    const std::string wrapped = command + " >/dev/null 2>&1 || { echo FAILED; exit 1; }";
    const std::string output = run_command_capture(wrapped);
    INFO(output);
    CHECK(output.find("FAILED") == std::string::npos);
}

void init_git_repository(const std::filesystem::path& path) {
    require_command_success(
        "mkdir -p " + escape_shell_arg(path.string()) +
        " && git init -b main " + escape_shell_arg(path.string()) +
        " && git -C " + escape_shell_arg(path.string()) + " config user.name test" +
        " && git -C " + escape_shell_arg(path.string()) + " config user.email test@example.test"
    );
}

std::string git_head_commit(const std::filesystem::path& path) {
    const std::string output = run_command_capture(
        "git -C " + escape_shell_arg(path.string()) + " rev-parse --verify HEAD"
    );
    std::istringstream input(output);
    std::string line;
    std::getline(input, line);
    return line;
}

void write_self_update_source_tree(const std::filesystem::path& root, const std::string& versionLabel) {
    write_file(root / "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.15)\n"
        "project(SelfUpdateStub LANGUAGES CXX)\n"
        "add_executable(ReqPack src/main.cpp)\n"
        "set_target_properties(ReqPack PROPERTIES OUTPUT_NAME rqp)\n");
    write_file(root / "src" / "main.cpp",
        "#include <iostream>\n"
        "int main() { std::cout << \"" + versionLabel + "\\n\"; return 0; }\n");
}

void commit_self_update_source(const std::filesystem::path& repoPath, const std::string& message, const std::string& versionLabel) {
    write_self_update_source_tree(repoPath, versionLabel);
    require_command_success(
        "git -C " + escape_shell_arg(repoPath.string()) + " add CMakeLists.txt src/main.cpp" +
        " && git -C " + escape_shell_arg(repoPath.string()) + " commit -m " + escape_shell_arg(message)
    );
}

void tag_git_commit(const std::filesystem::path& repoPath, const std::string& tagName) {
    require_command_success(
        "git -C " + escape_shell_arg(repoPath.string()) + " tag " + escape_shell_arg(tagName)
    );
}

void write_test_plugin_bundle(const std::filesystem::path& root, const std::string& pluginName, const std::string& versionLabel) {
    write_file(root / pluginName / (pluginName + ".lua"),
        "plugin = {}\n"
        "function plugin.getName() return '" + pluginName + "' end\n"
        "function plugin.getVersion() return '" + versionLabel + "' end\n"
        "function plugin.getRequirements() return {} end\n"
        "function plugin.getCategories() return { 'pkg', 'test' } end\n"
        "function plugin.getMissingPackages(packages) return packages end\n"
        "function plugin.install(context, packages) return true end\n"
        "function plugin.installLocal(context, path) return true end\n"
        "function plugin.remove(context, packages) return true end\n"
        "function plugin.update(context, packages) return true end\n"
        "function plugin.list(context) return { { name = '" + pluginName + "', version = '" + versionLabel + "', description = '" + versionLabel + "' } } end\n"
        "function plugin.search(context, prompt) return {} end\n"
        "function plugin.info(context, package) return { name = package, version = '" + versionLabel + "', description = '" + versionLabel + "' } end\n"
        "function plugin.shutdown() return true end\n");
}

void commit_plugin_source_version(
    const std::filesystem::path& repoPath,
    const std::string& pluginName,
    const std::string& message,
    const std::string& versionLabel,
    const std::optional<std::string>& tagName = std::nullopt
) {
    write_test_plugin_bundle(repoPath / "plugins", pluginName, versionLabel);
    require_command_success(
        "git -C " + escape_shell_arg(repoPath.string()) + " add plugins" +
        " && git -C " + escape_shell_arg(repoPath.string()) + " commit -m " + escape_shell_arg(message)
    );
    if (tagName.has_value()) {
        tag_git_commit(repoPath, tagName.value());
    }
}

std::string run_reqpack_with_stdin(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::vector<std::string>& arguments,
    const std::string& stdinContent
) {
    std::string command = "cd " + escape_shell_arg(workspace.string()) +
        " && printf %s " + escape_shell_arg(stdinContent) +
        " | " + escape_shell_arg((build_root() / "rqp").string()) +
        " --config " + escape_shell_arg(configPath.string());
    for (const std::string& argument : arguments) {
        command += " " + escape_shell_arg(argument);
    }
    command += " 2>&1";
    return run_command_capture(command);
}

std::string run_reqpack_with_home_and_stdin(
    const std::filesystem::path& workspace,
    const std::filesystem::path& configPath,
    const std::filesystem::path& homePath,
    const std::vector<std::string>& arguments,
    const std::string& stdinContent
) {
	std::string innerCommand =
		"printf %s " + escape_shell_arg(stdinContent) +
		" | " + escape_shell_arg((build_root() / "rqp").string()) +
		" --config " + escape_shell_arg(configPath.string());
	for (const std::string& argument : arguments) {
		innerCommand += " " + escape_shell_arg(argument);
	}

	std::string command = "cd " + escape_shell_arg(workspace.string()) +
		" && env HOME=" + escape_shell_arg(homePath.string()) +
		" XDG_CONFIG_HOME=" + escape_shell_arg((homePath / ".config").string()) +
		" XDG_DATA_HOME=" + escape_shell_arg((homePath / ".local" / "share").string()) +
		" XDG_CACHE_HOME=" + escape_shell_arg((homePath / ".cache").string()) +
		" sh -c " + escape_shell_arg(innerCommand);
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
        argvStrings.push_back((build_root() / "rqp").string());
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
        " && env HOME=" + escape_shell_arg(homePath.string()) +
        " XDG_CONFIG_HOME=" + escape_shell_arg((homePath / ".config").string()) +
        " XDG_DATA_HOME=" + escape_shell_arg((homePath / ".local" / "share").string()) +
        " XDG_CACHE_HOME=" + escape_shell_arg((homePath / ".cache").string()) +
        " " + escape_shell_arg((build_root() / "rqp").string()) +
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
        " && env HOME=" + escape_shell_arg(homePath.string()) +
        " XDG_CONFIG_HOME=" + escape_shell_arg((homePath / ".config").string()) +
        " XDG_DATA_HOME=" + escape_shell_arg((homePath / ".local" / "share").string()) +
        " XDG_CACHE_HOME=" + escape_shell_arg((homePath / ".cache").string());
    for (const auto& [key, value] : environment) {
        command += " " + key + "=" + escape_shell_arg(value);
    }
    command += " " + escape_shell_arg((build_root() / "rqp").string()) +
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
      type = "doc",
      architecture = "noarch",
      description = "listed from " .. context.plugin.dir,
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
      description = "outdated from " .. context.plugin.id,
    }
  }
end
function plugin.search(context, prompt)
  return {
    {
      name = prompt,
      version = "2.0.0",
      type = "doc",
      architecture = "noarch",
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

const char* ORCHESTRATOR_ARCHIVE_COPY_PLUGIN = R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  return context.exec.run("test -d '" .. path .. "' && cp '" .. path .. "/artifact.txt' '" .. state .. "/artifact.txt'").success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)";

const char* SYSTEM_WIDE_UPDATE_PLUGIN = R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages)
  local path = context.plugin.dir .. "/state/update.txt"
  local value = tostring(#packages)
  context.exec.run("mkdir -p '" .. context.plugin.dir .. "/state' && printf '%s' '" .. value .. "' > '" .. path .. "'")
  return true
end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0", description = "ok" } end
function plugin.shutdown() return true end
)";

const char* SBOM_INFO_ONLY_PLUGIN = R"(
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
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context)
  context.events.listed({ { name = "unexpected-list-call", version = "0.0.0", description = "resolver should not call list" } })
  return {
    {
      name = "unexpected-list-call",
      version = "0.0.0",
      description = "resolver should not call list",
    }
  }
end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  reqpack.exec.run("printf 'resolver-exec'")
  if package == "resolved" then
    local item = {
      name = package,
      version = "4.5.6",
      description = "resolved via info",
    }
    context.events.informed(item)
    return item
  end

  local item = {
    name = package,
    version = "unknown",
    description = "not installed",
  }
  context.events.informed(item)
  return item
end
function plugin.resolvePackage(context, package)
  reqpack.exec.run("printf 'resolver-exec'")
  if package.name == "resolved" then
    package.version = "4.5.6"
    return package
  end
  return nil
end
function plugin.shutdown() return true end
)";

const char* SBOM_RESOLVE_PLUGIN = R"(
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
function plugin.getMissingPackages(packages)
  local missing = {}
  for _, package in ipairs(packages) do
    missing[#missing + 1] = package
  end
  return missing
end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package)
  return {
    name = package,
    version = "unknown",
    description = "resolvePackage should decide existence",
  }
end
function plugin.resolvePackage(context, package)
  if package.name == "resolved" then
    package.version = "4.5.6"
    return package
  end
  return nil
end
function plugin.shutdown() return true end
)";

const char* PROXY_PLUGIN = R"(
plugin = {}

function plugin.getName() return "java-proxy" end
function plugin.getVersion() return "1.0.0" end
function plugin.getSecurityMetadata()
  return {
    osvEcosystem = "Maven",
    purlType = "generic",
    versionComparatorProfile = "lexicographic",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "proxy", "java" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return false end
function plugin.installLocal(context, path) return false end
function plugin.remove(context, packages) return false end
function plugin.update(context, packages) return false end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "proxy" } end
function plugin.resolveProxyRequest(context, request)
  local target = context.proxy.default
  if target == nil or target == "" then
    target = context.proxy.targets[1]
  end
  return {
    targetSystem = target,
    packages = request.packages,
    flags = request.flags,
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

    CHECK(output.find("LIST: workspace") != std::string::npos);
    CHECK(output.find("1.0.0") != std::string::npos);
    CHECK(output.find("doc") != std::string::npos);
    CHECK(output.find("noarch") != std::string::npos);
    CHECK(output.find("listed from") != std::string::npos);
    CHECK(output.find("workspace") != std::string::npos);
}

TEST_CASE("orchestrator outdated command prints normalized latest version columns", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-outdated"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "query", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"outdated", "query"});

    CHECK(output.find("OUTDATED: query") != std::string::npos);
    CHECK(output.find("Installed") != std::string::npos);
    CHECK(output.find("Latest") != std::string::npos);
    CHECK(output.find("1.0.0") != std::string::npos);
    CHECK(output.find("2.0.0") != std::string::npos);
    CHECK(output.find("doc") != std::string::npos);
    CHECK(output.find("noarch") != std::string::npos);
    CHECK(output.find("outdated from") != std::string::npos);
    CHECK(output.find("query") != std::string::npos);
}

TEST_CASE("orchestrator dnf list normalizes metadata and filters architecture", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-dnf-list-arch-filter"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "dnf");

    write_file(fakeBin / "dnf",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"repoquery\" ] && [ \"$2\" = \"--installed\" ]; then\n"
        "  if printf '%s' \"$*\" | grep -F -- '%{summary}' >/dev/null 2>&1; then\n"
        "    printf 'ripgrep.x86_64\\tFast line-oriented search tool\\nripgrep.noarch\\tFast line-oriented search tool\\n'\n"
        "    exit 0\n"
        "  fi\n"
        "  printf 'ripgrep.x86_64\\t14.1.1-1.fc43\\nripgrep.noarch\\t14.1.1-1.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dnf").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"COLUMNS", "120"},
        },
        {"list", "dnf", "--arch", "x86_64"},
        status
    );
    INFO(output);

    CHECK(status == 0);
    CHECK(output.find("LIST: dnf") != std::string::npos);
    CHECK(output.find("ripgrep") != std::string::npos);
    CHECK(output.find("14.1.1-1.fc43") != std::string::npos);
    CHECK(output.find("package") != std::string::npos);
    CHECK(output.find("x86_64") != std::string::npos);
    CHECK(output.find("Fast line-oriented") != std::string::npos);
    CHECK(output.find("search tool") != std::string::npos);
    CHECK(output.find("Installed RPM") == std::string::npos);
    CHECK(output.find("noarch") == std::string::npos);
}

TEST_CASE("orchestrator dnf outdated shows installed and latest versions", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-dnf-outdated"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "dnf");

    write_file(fakeBin / "dnf",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"check-update\" ]; then\n"
        "  printf 'ripgrep.x86_64 15.0.0-1.fc43 updates\\n'\n"
        "  exit 100\n"
        "fi\n"
        "if [ \"$1\" = \"repoquery\" ] && [ \"$2\" = \"--installed\" ]; then\n"
        "  printf 'ripgrep.x86_64\\tFast line-oriented search tool\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"repoquery\" ]; then\n"
        "  printf 'ripgrep.x86_64\\t15.0.0-1.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 0\n");
    write_file(fakeBin / "rpm",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-q\" ] && [ \"$2\" = \"--qf\" ]; then\n"
        "  printf '14.1.1-1.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"-q\" ] && [ \"$2\" = \"--quiet\" ]; then\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"-q\" ] && [ \"$2\" = \"--whatprovides\" ]; then\n"
        "  printf 'ripgrep\\t14.1.1-1.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dnf").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "rpm").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"COLUMNS", "120"},
        },
        {"outdated", "dnf"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("OUTDATED: dnf") != std::string::npos);
    CHECK(output.find("Installed") != std::string::npos);
    CHECK(output.find("Latest") != std::string::npos);
    CHECK(output.find("ripgrep") != std::string::npos);
    CHECK(output.find("14.1.1-1.fc43") != std::string::npos);
    CHECK(output.find("15.0.0-1.fc43") != std::string::npos);
    CHECK(output.find("package") != std::string::npos);
    CHECK(output.find("x86_64") != std::string::npos);
    CHECK(output.find("Fast line-oriented") != std::string::npos);
    CHECK(output.find("search tool") != std::string::npos);
}

TEST_CASE("orchestrator maven list normalizes type metadata without fake status description", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-maven-list"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path repoRoot = tempDir.path() / ".m2" / "repository";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "maven");

    write_file(fakeBin / "java",
        "#!/bin/sh\n"
        "exit 0\n");
    write_file(fakeBin / "mvn",
        "#!/bin/sh\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "java").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "mvn").string())).c_str()) == 0);

    write_file(repoRoot / "org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom",
        "<project>\n"
        "  <modelVersion>4.0.0</modelVersion>\n"
        "  <groupId>org.example</groupId>\n"
        "  <artifactId>demo-lib</artifactId>\n"
        "  <version>1.2.3</version>\n"
        "  <description>Demo library artifact</description>\n"
        "</project>\n");
    write_file(repoRoot / "org/example/demo-bom/2.0.0/demo-bom-2.0.0.pom",
        "<project>\n"
        "  <modelVersion>4.0.0</modelVersion>\n"
        "  <groupId>org.example</groupId>\n"
        "  <artifactId>demo-bom</artifactId>\n"
        "  <version>2.0.0</version>\n"
        "  <packaging>pom</packaging>\n"
        "  <description>Demo bill of materials</description>\n"
        "</project>\n");

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_MAVEN_REPO", repoRoot.string()},
            {"COLUMNS", "120"},
        },
        {"list", "maven"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("LIST: maven") != std::string::npos);
    CHECK(output.find("demo-lib") != std::string::npos);
    CHECK(output.find("1.2.3") != std::string::npos);
    CHECK(output.find("Type") != std::string::npos);
    CHECK(output.find("jar") != std::string::npos);
    CHECK(output.find("Demo") != std::string::npos);
    CHECK(output.find("artifact") != std::string::npos);
    CHECK(output.find("demo-bom") != std::string::npos);
    CHECK(output.find("2.0.0") != std::string::npos);
    CHECK(output.find("pom") != std::string::npos);
    CHECK(output.find("materials") != std::string::npos);
    CHECK(output.find("Installed in local Maven repository") == std::string::npos);
}

TEST_CASE("orchestrator maven outdated shows latest version once per artifact", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-maven-outdated"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path repoRoot = tempDir.path() / ".m2" / "repository";
    const std::filesystem::path curlLog = tempDir.path() / "curl.log";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "maven");

    write_file(fakeBin / "java",
        "#!/bin/sh\n"
        "exit 0\n");
    write_file(fakeBin / "mvn",
        "#!/bin/sh\n"
        "exit 0\n");
    write_file(fakeBin / "curl",
        "#!/bin/sh\n"
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(curlLog.string()) + "\n"
        "case \"$*\" in\n"
        "  *'g:org.example+AND+a:demo-lib'*)\n"
        "    printf '{\"response\":{\"docs\":[{\"latestVersion\":\"1.5.0\"}]}}'\n"
        "    ;;\n"
        "  *)\n"
        "    exit 1\n"
        "    ;;\n"
        "esac\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "java").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "mvn").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "curl").string())).c_str()) == 0);

    write_file(repoRoot / "org/example/demo-lib/1.2.3/demo-lib-1.2.3.pom",
        "<project>\n"
        "  <modelVersion>4.0.0</modelVersion>\n"
        "  <groupId>org.example</groupId>\n"
        "  <artifactId>demo-lib</artifactId>\n"
        "  <version>1.2.3</version>\n"
        "  <description>Demo library artifact</description>\n"
        "</project>\n");
    write_file(repoRoot / "org/example/demo-lib/1.5.0/demo-lib-1.5.0.pom",
        "<project>\n"
        "  <modelVersion>4.0.0</modelVersion>\n"
        "  <groupId>org.example</groupId>\n"
        "  <artifactId>demo-lib</artifactId>\n"
        "  <version>1.5.0</version>\n"
        "  <description>Demo library artifact</description>\n"
        "</project>\n");

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_MAVEN_REPO", repoRoot.string()},
            {"COLUMNS", "120"},
        },
        {"outdated", "maven"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("OUTDATED: maven") != std::string::npos);
    CHECK(output.find("org.example:demo-lib") == std::string::npos);

    std::filesystem::remove(repoRoot / "org/example/demo-lib/1.5.0/demo-lib-1.5.0.pom");

    const std::string secondOutput = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_MAVEN_REPO", repoRoot.string()},
            {"COLUMNS", "120"},
        },
        {"outdated", "maven"},
        status
    );

    CHECK(status == 0);
    CHECK(secondOutput.find("OUTDATED: maven") != std::string::npos);
    CHECK(secondOutput.find("demo-lib") != std::string::npos);
    CHECK(secondOutput.find("1.2.3") != std::string::npos);
    CHECK(secondOutput.find("1.5.0") != std::string::npos);
    CHECK(secondOutput.find("jar") != std::string::npos);
    CHECK(secondOutput.find("Demo") != std::string::npos);
    CHECK(secondOutput.find("artifact") != std::string::npos);
    CHECK(secondOutput.find("Newer version available") == std::string::npos);
}

TEST_CASE("orchestrator search command prints executor search results", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-search"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "query", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"search", "query", "alpha", "beta"});

    CHECK(output.find("SEARCH: query") != std::string::npos);
    CHECK(output.find("alpha beta") != std::string::npos);
    CHECK(output.find("2.0.0") != std::string::npos);
    CHECK(output.find("doc") != std::string::npos);
    CHECK(output.find("noarch") != std::string::npos);
    CHECK(output.find("searched by query") != std::string::npos);
}

TEST_CASE("orchestrator info command prints executor info result", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-info"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "query", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"info", "query", "sample", "ignored"});

    CHECK(output.find("INFO: query") != std::string::npos);
    CHECK(output.find("Name") != std::string::npos);
    CHECK(output.find("sample") != std::string::npos);
    CHECK(output.find("Version") != std::string::npos);
    CHECK(output.find("3.0.0") != std::string::npos);
    CHECK(output.find("info from query") != std::string::npos);
}

TEST_CASE("orchestrator dnf search parses indented native dnf results", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-dnf-search"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "dnf");

    write_file(fakeBin / "dnf",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"search\" ]; then\n"
        "  printf 'Matched fields: name (exact)\\n texlive-xurl.noarch\\tAllow url break at any alphanumerical character\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"repoquery\" ]; then\n"
        "  printf 'texlive-xurl.noarch\\tsvn61553-80.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dnf").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
        },
        {"search", "dnf", "texlive-xurl"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("SEARCH: dnf") != std::string::npos);
    CHECK(output.find("texlive-xurl") != std::string::npos);
    CHECK(output.find("package") != std::string::npos);
    CHECK(output.find("noarch") != std::string::npos);
    CHECK(output.find("svn61553-80.fc43") != std::string::npos);
    CHECK(output.find("Allow url break") != std::string::npos);
    CHECK(output.find("alphanumerical") != std::string::npos);
    CHECK(output.find("No results") == std::string::npos);
}

TEST_CASE("orchestrator dnf search honors arch filter", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-dnf-search-arch-filter"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    std::filesystem::create_directories(fakeBin);

    copy_repo_plugin(pluginDirectory, "dnf");

    write_file(fakeBin / "dnf",
        "#!/bin/sh\n"
        "if [ \"$1\" = \"search\" ]; then\n"
        "  printf 'Matched fields: name (exact)\\n texlive-xurl.noarch\\tAllow url break at any alphanumerical character\\n texlive-xurl.x86_64\\tAllow url break at any alphanumerical character\\n'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$1\" = \"repoquery\" ]; then\n"
        "  printf 'texlive-xurl.noarch\\tsvn61553-80.fc43\\ntexlive-xurl.x86_64\\tsvn61553-80.fc43\\n'\n"
        "  exit 0\n"
        "fi\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "dnf").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
        },
        {"search", "dnf", "texlive-xurl", "--arch", "x86_64"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("texlive-xurl") != std::string::npos);
    CHECK(output.find("x86_64") != std::string::npos);
    CHECK(output.find("noarch") == std::string::npos);
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

TEST_CASE("orchestrator resolves proxy plugin install requests to configured target", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-proxy-install"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_proxy(tempDir.path(), pluginDirectory, "maven", "{ 'maven', 'gradle' }");

    add_plugin_script(pluginDirectory, "java", PROXY_PLUGIN);
    add_plugin_script(pluginDirectory, "maven", ORCHESTRATOR_PLUGIN);
    add_plugin_script(pluginDirectory, "gradle", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "java", "sample"});
    INFO(output);

    const std::filesystem::path mavenInstallMarker = pluginDirectory / "maven" / "state" / "install.txt";
    const std::filesystem::path gradleInstallMarker = pluginDirectory / "gradle" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(mavenInstallMarker));
    CHECK(read_file(mavenInstallMarker) == "sample");
    CHECK_FALSE(std::filesystem::exists(gradleInstallMarker));
}

TEST_CASE("orchestrator proxy default target can be overridden from CLI define flag", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-proxy-override"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_proxy(tempDir.path(), pluginDirectory, "maven", "{ 'maven', 'gradle' }");

    add_plugin_script(pluginDirectory, "java", PROXY_PLUGIN);
    add_plugin_script(pluginDirectory, "maven", ORCHESTRATOR_PLUGIN);
    add_plugin_script(pluginDirectory, "gradle", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "java", "sample", "-Dproxy.java.default=gradle"});
    INFO(output);

    const std::filesystem::path mavenInstallMarker = pluginDirectory / "maven" / "state" / "install.txt";
    const std::filesystem::path gradleInstallMarker = pluginDirectory / "gradle" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(gradleInstallMarker));
    CHECK(read_file(gradleInstallMarker) == "sample");
    CHECK_FALSE(std::filesystem::exists(mavenInstallMarker));
}

TEST_CASE("orchestrator resolves proxy plugin search requests before logging output", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-proxy-search"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_proxy(tempDir.path(), pluginDirectory, "maven", "{ 'maven' }");

    add_plugin_script(pluginDirectory, "java", PROXY_PLUGIN);
    add_plugin_script(pluginDirectory, "maven", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"search", "java", "alpha", "beta"});

    CHECK(output.find("SEARCH: maven") != std::string::npos);
    CHECK(output.find("alpha beta") != std::string::npos);
    CHECK(output.find("2.0.0") != std::string::npos);
    CHECK(output.find("searched by maven") != std::string::npos);
}

TEST_CASE("orchestrator loads repo java proxy plugin from workspace and routes install to configured target", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-repo-java-proxy"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_proxy(tempDir.path(), pluginDirectory, "maven", "{ 'maven', 'gradle' }");

    copy_repo_plugin(pluginDirectory, "java");
    add_plugin_script(pluginDirectory, "maven", ORCHESTRATOR_PLUGIN);
    add_plugin_script(pluginDirectory, "gradle", ORCHESTRATOR_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "java", "sample"});
    INFO(output);

    const std::filesystem::path mavenInstallMarker = pluginDirectory / "maven" / "state" / "install.txt";
    const std::filesystem::path gradleInstallMarker = pluginDirectory / "gradle" / "state" / "install.txt";
    REQUIRE(std::filesystem::exists(mavenInstallMarker));
    CHECK(read_file(mavenInstallMarker) == "sample");
    CHECK_FALSE(std::filesystem::exists(gradleInstallMarker));
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

TEST_CASE("orchestrator install local archive passes extracted directory to plugin", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-archive"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  local copy = context.exec.run("test -d '" .. path .. "' && cp '" .. path .. "/artifact.txt' '" .. state .. "/artifact.txt' && cp '" .. path .. "/nested/inner.txt' '" .. state .. "/inner.txt'")
  return copy.success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)" );

    const std::filesystem::path archivePath = build_zip_archive(
        tempDir.path(),
        "artifact.zip",
        {{"artifact.txt", "hello-local-archive"}, {"nested/inner.txt", "nested"}}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "apply", archivePath.string()});
    const std::filesystem::path marker = pluginDirectory / "apply" / "state" / "local.txt";
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-local-archive");
    CHECK(read_file(pluginDirectory / "apply" / "state" / "inner.txt") == "nested");
}

TEST_CASE("orchestrator install file-url archive resolves system after extraction", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-file-url-archive"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}
plugin.fileExtensions = { ".txt" }
function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return false end
function plugin.installLocal(context, path)
  return context.exec.run("test -d '" .. path .. "' && test -f '" .. path .. "/artifact.txt'").success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)" );

    const std::filesystem::path archivePath = build_zip_archive(tempDir.path(), "remote-artifact.zip", {{"artifact.txt", "hello-url-archive"}});
    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "file://" + archivePath.string()});
    INFO(output);
    CHECK(output.find("INSTALL: apply:local") != std::string::npos);
    CHECK(output.find("1 ok") != std::string::npos);
}

TEST_CASE("orchestrator install encrypted local archive accepts CLI password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-archive-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  local copy = context.exec.run("test -d '" .. path .. "' && cp '" .. path .. "/artifact.txt' '" .. state .. "/artifact.txt'")
  return copy.success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)");

    const std::filesystem::path archivePath = build_encrypted_zip_archive(
        tempDir.path(),
        "artifact-encrypted-cli.zip",
        "secret123",
        {{"artifact.txt", "hello-encrypted-cli"}}
    );

    const std::string output = run_reqpack(
        tempDir.path(),
        configPath,
        {"--archive-password", "secret123", "install", "apply", archivePath.string()}
    );
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-encrypted-cli");
}

TEST_CASE("orchestrator install encrypted local archive accepts config password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-archive-config"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  return context.exec.run("test -d '" .. path .. "' && cp '" .. path .. "/artifact.txt' '" .. state .. "/artifact.txt'").success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)");

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
        "  archives = {\n"
        "    password = 'secret456',\n"
        "  },\n"
        "  rqp = {\n"
        "    statePath = '" + (tempDir.path() / "rqp-state").string() + "',\n"
        "  },\n"
        "}\n");

    const std::filesystem::path archivePath = build_encrypted_zip_archive(
        tempDir.path(),
        "artifact-encrypted-config.zip",
        "secret456",
        {{"artifact.txt", "hello-encrypted-config"}}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "apply", archivePath.string()});
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-encrypted-config");
}

TEST_CASE("orchestrator install encrypted local archive accepts env password fallback", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-archive-env"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    int status = 0;

    add_plugin_script(pluginDirectory, "apply", R"(
plugin = {}

function plugin.getName() return REQPACK_PLUGIN_ID end
function plugin.getVersion() return "1.0.0" end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "pkg", "orch" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path)
  local state = context.plugin.dir .. "/state"
  context.exec.run("mkdir -p '" .. state .. "'")
  return context.exec.run("test -d '" .. path .. "' && cp '" .. path .. "/artifact.txt' '" .. state .. "/artifact.txt'").success
end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "1.0.0" } end
function plugin.shutdown() return true end
)");

    const std::filesystem::path archivePath = build_encrypted_zip_archive(
        tempDir.path(),
        "artifact-encrypted-env.zip",
        "secret789",
        {{"artifact.txt", "hello-encrypted-env"}}
    );

    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {{"REQPACK_ARCHIVE_PASSWORD", "secret789"}},
        {"install", "apply", archivePath.string()},
        status
    );
    INFO(output);
    CHECK(status == 0);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-encrypted-env");
}

TEST_CASE("orchestrator install encrypted local archive fails without password in non-interactive mode", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-archive-missing-password"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    int status = 0;

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::filesystem::path archivePath = build_encrypted_zip_archive(
        tempDir.path(),
        "artifact-encrypted-missing.zip",
        "secret000",
        {{"artifact.txt", "hello-encrypted-missing"}}
    );

    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {},
        {"install", "apply", archivePath.string()},
        status
    );
    INFO(output);
    CHECK(status != 0);
    CHECK(output.find("archive password required: " + archivePath.string()) != std::string::npos);
}

TEST_CASE("orchestrator install encrypted local archive fails on invalid password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-archive-invalid-password"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    int status = 0;

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::filesystem::path archivePath = build_encrypted_zip_archive(
        tempDir.path(),
        "artifact-encrypted-invalid.zip",
        "secret111",
        {{"artifact.txt", "hello-encrypted-invalid"}}
    );

    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {},
        {"--archive-password", "wrong", "install", "apply", archivePath.string()},
        status
    );
    INFO(output);
    CHECK(status != 0);
    CHECK(output.find("invalid archive password: " + archivePath.string()) != std::string::npos);
}

TEST_CASE("orchestrator install encrypted seven zip archive accepts CLI password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-encrypted-7z-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "seven-zip-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-encrypted-7z"))
    );

    const std::filesystem::path archivePath = build_encrypted_seven_zip_archive(
        tempDir.path(),
        "artifact-encrypted.7z",
        "secret7z",
        {{localPackage.filename().string(), read_file(localPackage)}}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"--archive-password", "secret7z", "install", "rqp", archivePath.string()});
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "seven-zip-artifact" / "seven-zip-artifact@1.0.0-1+r0";
    INFO(output);
    CHECK(std::filesystem::exists(stateDir / "installed.txt"));
    CHECK(read_file(stateDir / "installed.txt") == "hello-encrypted-7z");
}

TEST_CASE("orchestrator install gpg wrapped zip archive accepts CLI password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-zip-gpg-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_ARCHIVE_COPY_PLUGIN);

    const std::filesystem::path innerArchivePath = build_zip_archive(
        tempDir.path(),
        "artifact-inner.zip",
        {{"artifact.txt", "hello-zip-gpg"}}
    );
    const std::filesystem::path archivePath = wrap_archive_with_gpg(tempDir.path(), innerArchivePath, "artifact.zip.gpg", "secretgpg");

    const std::string output = run_reqpack(tempDir.path(), configPath, {"--archive-password", "secretgpg", "install", "apply", archivePath.string()});
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-zip-gpg");
}

TEST_CASE("orchestrator install gpg wrapped tar gz archive accepts CLI password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-targz-gpg-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_ARCHIVE_COPY_PLUGIN);

    const std::filesystem::path innerArchivePath = build_tar_gz_archive(
        tempDir.path(),
        "artifact-inner.tar.gz",
        {{"artifact.txt", "hello-targz-gpg"}}
    );
    const std::filesystem::path archivePath = wrap_archive_with_gpg(tempDir.path(), innerArchivePath, "artifact.tar.gz.gpg", "secret-targz");

    const std::string output = run_reqpack(tempDir.path(), configPath, {"--archive-password", "secret-targz", "install", "apply", archivePath.string()});
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-targz-gpg");
}

TEST_CASE("orchestrator install gpg wrapped seven zip archive accepts CLI password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-7z-gpg-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_ARCHIVE_COPY_PLUGIN);

    const std::filesystem::path innerArchivePath = build_encrypted_seven_zip_archive(
        tempDir.path(),
        "artifact-inner.7z",
        "shared7zpass",
        {{"artifact.txt", "hello-7z-gpg"}}
    );
    const std::filesystem::path archivePath = wrap_archive_with_gpg(tempDir.path(), innerArchivePath, "artifact.7z.gpg", "shared7zpass");

    const std::string output = run_reqpack(tempDir.path(), configPath, {"--archive-password", "shared7zpass", "install", "apply", archivePath.string()});
    INFO(output);
    CHECK(read_file(pluginDirectory / "apply" / "state" / "artifact.txt") == "hello-7z-gpg");
}

TEST_CASE("orchestrator install gpg wrapped archive fails on invalid password", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-local-gpg-invalid-password"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    int status = 0;

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    const std::filesystem::path innerArchivePath = build_zip_archive(
        tempDir.path(),
        "artifact-invalid-inner.zip",
        {{"artifact.txt", "hello-invalid-gpg"}}
    );
    const std::filesystem::path archivePath = wrap_archive_with_gpg(tempDir.path(), innerArchivePath, "artifact-invalid.zip.gpg", "rightpass");

    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {},
        {"--archive-password", "wrongpass", "install", "apply", archivePath.string()},
        status
    );
    INFO(output);
    CHECK(status != 0);
    CHECK(output.find("invalid archive password: " + archivePath.string()) != std::string::npos);
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

TEST_CASE("orchestrator install named rqp package resolves repository zip artifact", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-install-rqp-repo-zip"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path artifactPath = build_rqp_package(
        tempDir.path(),
        "repo-zipped-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-from-zipped-repo"))
    );
    const std::filesystem::path archivePath = build_zip_archive(
        tempDir.path(),
        "repo-zipped-artifact.zip",
        {{artifactPath.filename().string(), read_file(artifactPath)}}
    );
    const std::filesystem::path indexPath = write_rq_repository_index(
        tempDir.path(),
        "repo-zipped-artifact",
        "1.0.0",
        archivePath
    );
    const std::filesystem::path configPath = write_config_with_rq_repositories(
        tempDir.path(),
        pluginDirectory,
        {"file://" + indexPath.string()}
    );

    const std::string output = run_reqpack(tempDir.path(), configPath, {"install", "rqp:repo-zipped-artifact@1.0.0"});
    const std::filesystem::path stateDir = tempDir.path() / "rqp-state" / "repo-zipped-artifact" / "repo-zipped-artifact@1.0.0-1+r0";
    INFO(output);
    CHECK(std::filesystem::exists(stateDir / "installed.txt"));
    CHECK(read_file(stateDir / "installed.txt") == "hello-from-zipped-repo");
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

    CHECK(listOutput.find("LIST: rqp") != std::string::npos);
    CHECK(listOutput.find("listed-artifact") != std::string::npos);
    CHECK(listOutput.find("1.0.0-1+r0") != std::string::npos);
    CHECK(listOutput.find("test package") != std::string::npos);
    CHECK(infoOutput.find("INFO: rqp") != std::string::npos);
    CHECK(infoOutput.find("listed-artifact") != std::string::npos);
    CHECK(infoOutput.find("1.0.0-1+r0") != std::string::npos);
    CHECK(infoOutput.find("test package") != std::string::npos);
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

TEST_CASE("wrapper self-update builds latest git commit and swaps local symlink", "[integration][orchestrator][service][self-update]") {
    TempDir tempDir{"reqpack-wrapper-self-update"};
    const std::filesystem::path workspace = tempDir.path() / "workspace";
    const std::filesystem::path homePath = tempDir.path() / "home";
    const std::filesystem::path originPath = tempDir.path() / "origin";
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path fakeBinDirectory = tempDir.path() / "fake-bin";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(homePath);
    std::filesystem::create_directories(fakeBinDirectory);

    init_git_repository(originPath);
    commit_self_update_source(originPath, "initial", "v1");
    copy_repo_plugin(pluginDirectory, "sys");
    write_file(fakeBinDirectory / "fake-apt", "#!/bin/sh\nprintf '%s\n' \"$@\" >> \"$REQPACK_TEST_SYS_LOG\"\nexit 0\n");
    require_command_success("chmod +x " + escape_shell_arg((fakeBinDirectory / "fake-apt").string()));

    write_file(configPath,
        "return {\n"
        "  logging = {\n"
        "    consoleOutput = true,\n"
        "  },\n"
        "  planner = {\n"
        "    autoDownloadMissingPlugins = false,\n"
        "    autoDownloadMissingDependencies = false,\n"
        "  },\n"
        "  registry = {\n"
        "    pluginDirectory = '" + pluginDirectory.string() + "',\n"
        "    databasePath = '" + (tempDir.path() / "registry-db").string() + "',\n"
        "    autoLoadPlugins = true,\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "  selfUpdate = {\n"
        "    repoUrl = '" + originPath.string() + "',\n"
        "    branch = 'main',\n"
        "    repoPath = '" + (homePath / ".local/share/reqpack/self/repo").string() + "',\n"
        "    buildPath = '" + (homePath / ".local/share/reqpack/self/build").string() + "',\n"
        "    binaryDirectory = '" + (homePath / ".local/share/reqpack/self/bin").string() + "',\n"
        "    linkPath = '" + (homePath / ".local/bin/rqp").string() + "',\n"
        "  },\n"
        "}\n");

    const std::string firstCommit = git_head_commit(originPath);
    const std::string firstOutput = run_reqpack_with_home(workspace, configPath, homePath, {"update"});
    INFO(firstOutput);
    CHECK(firstOutput.find("self-update: clone repository") != std::string::npos);
    CHECK(firstOutput.find("self-update: configure build") != std::string::npos);
    CHECK(firstOutput.find("self-update: build binary") != std::string::npos);
    CHECK(firstOutput.find("self-update complete: now on commit " + firstCommit) != std::string::npos);

    const std::filesystem::path linkPath = homePath / ".local/bin/rqp";
    const std::filesystem::path binaryPath = homePath / ".local/share/reqpack/self/bin" / ("rqp-" + firstCommit);
    REQUIRE(std::filesystem::exists(binaryPath));
    REQUIRE(std::filesystem::is_symlink(linkPath));
    CHECK(std::filesystem::read_symlink(linkPath) == binaryPath);

    commit_self_update_source(originPath, "second", "v2");
    const std::string secondCommit = git_head_commit(originPath);
    const std::string secondOutput = run_reqpack_with_home(workspace, configPath, homePath, {"update"});
    INFO(secondOutput);
    CHECK(secondOutput.find("self-update: checkout branch") != std::string::npos);
    CHECK(secondOutput.find("self-update: pull latest commit") != std::string::npos);
    CHECK(secondOutput.find("self-update complete: now on commit " + secondCommit) != std::string::npos);

    const std::filesystem::path secondBinaryPath = homePath / ".local/share/reqpack/self/bin" / ("rqp-" + secondCommit);
    REQUIRE(std::filesystem::exists(secondBinaryPath));
    CHECK(std::filesystem::read_symlink(linkPath) == secondBinaryPath);

    const std::filesystem::path sysLogPath = tempDir.path() / "sys-update.log";
    const std::string wrapperUpdateOutput = run_reqpack_with_home_and_env(
        workspace,
        configPath,
        homePath,
        {
            {"REQPACK_SYS_BACKEND", "apt"},
            {"REQPACK_SYS_APT_BIN", (fakeBinDirectory / "fake-apt").string()},
            {"REQPACK_SYS_APT_CACHE_BIN", (fakeBinDirectory / "fake-apt").string()},
            {"REQPACK_SYS_DPKG_QUERY_BIN", (fakeBinDirectory / "fake-apt").string()},
            {"REQPACK_SYS_NO_SUDO", "1"},
            {"REQPACK_TEST_SYS_LOG", sysLogPath.string()},
        },
        {"update", "sys", "pip"}
    );
    INFO(wrapperUpdateOutput);
    CHECK(wrapperUpdateOutput.find("self-update:") == std::string::npos);
    CHECK(wrapperUpdateOutput.find("UPDATE: sys:pip") != std::string::npos);
    CHECK(wrapperUpdateOutput.find("UPDATE done:  1 ok") != std::string::npos);
    const std::string sysLog = read_file(sysLogPath);
    CHECK(sysLog.find("install") != std::string::npos);
    CHECK(sysLog.find("--only-upgrade") != std::string::npos);
    CHECK(sysLog.find("python3-pip") != std::string::npos);
}

TEST_CASE("host refresh rewrites cached host snapshot", "[integration][orchestrator][service][host]") {
    TempDir tempDir{"reqpack-host-refresh"};
    const std::filesystem::path workspace = tempDir.path() / "workspace";
    const std::filesystem::path homePath = tempDir.path() / "home";
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(homePath);

    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path cachePath = homePath / ".cache" / "reqpack" / "host" / "info.v1.json";
    write_file(cachePath, "{ invalid json }");

    const std::string output = run_reqpack_with_home(workspace, configPath, homePath, {"host", "refresh"});
    INFO(output);
    CHECK(output.find("host refresh: cache updated") != std::string::npos);
    CHECK(output.find("host cache: " + cachePath.string()) != std::string::npos);

    const std::optional<HostInfoSnapshot> snapshot = read_host_info_snapshot_file(cachePath);
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->platform.osFamily.empty());
    CHECK_FALSE(snapshot->platform.arch.empty());
    CHECK(snapshot->cache.source == "cache");
    CHECK(snapshot->cache.refreshReason == "manual-live-probe");
}

TEST_CASE("update plugin refreshes git-backed wrapper to newest tagged version", "[integration][orchestrator][service][plugin-update]") {
    TempDir tempDir{"reqpack-plugin-wrapper-update"};
    const std::filesystem::path workspace = tempDir.path() / "workspace";
    const std::filesystem::path pluginRepoPath = tempDir.path() / "plugin-origin";
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    std::filesystem::create_directories(workspace);

    init_git_repository(pluginRepoPath);
    commit_plugin_source_version(pluginRepoPath, "pip", "v1", "1.0.0", "v1.0.0");
    commit_plugin_source_version(pluginRepoPath, "pip", "v2", "1.2.0", "v1.2.0");
    commit_plugin_source_version(pluginRepoPath, "pip", "head", "9.9.9-dev");

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
        "    sources = {\n"
        "      pip = { source = 'git+" + pluginRepoPath.string() + "' },\n"
        "    },\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "}\n");

    {
        ReqPackConfig defaults = default_reqpack_config();
        defaults.registry.remoteUrl.clear();
        Registry registry(load_config_from_lua(configPath, defaults));
        REQUIRE(registry.getDatabase()->ensureReady());
        REQUIRE(registry.loadPlugin("pip"));
        REQUIRE(registry.getPlugin("pip") != nullptr);
        CHECK(registry.getPlugin("pip")->getVersion() == "9.9.9-dev");
    }

    const std::string output = run_reqpack(workspace, configPath, {"update", "pip"});
    INFO(output);
    CHECK(output.find("UPDATE: pip") != std::string::npos);
    CHECK(output.find("UPDATE done:  1 ok") != std::string::npos);
    const std::string refreshedScript = read_file(pluginDirectory / "pip" / "pip.lua");
    CHECK(refreshedScript.find("1.2.0") != std::string::npos);
}

TEST_CASE("update system --all calls plugin update with empty package list", "[integration][orchestrator][service][plugin-update]") {
    TempDir tempDir{"reqpack-system-update-all"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    add_plugin_script(pluginDirectory, "apt", SYSTEM_WIDE_UPDATE_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {"update", "apt", "--all"});
    INFO(output);
    CHECK(output.find("UPDATE: apt") != std::string::npos);
    CHECK(output.find("UPDATE done:  1 ok") != std::string::npos);
    CHECK(read_file(pluginDirectory / "apt" / "state" / "update.txt") == "0");
}

TEST_CASE("update --all refreshes all known plugin wrappers", "[integration][orchestrator][service][plugin-update]") {
    TempDir tempDir{"reqpack-plugin-wrapper-update-all"};
    const std::filesystem::path workspace = tempDir.path() / "workspace";
    const std::filesystem::path pipRepoPath = tempDir.path() / "pip-origin";
    const std::filesystem::path npmRepoPath = tempDir.path() / "npm-origin";
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    std::filesystem::create_directories(workspace);

    init_git_repository(pipRepoPath);
    commit_plugin_source_version(pipRepoPath, "pip", "v1", "1.0.0", "v1.0.0");
    commit_plugin_source_version(pipRepoPath, "pip", "v2", "1.2.0", "v1.2.0");
    commit_plugin_source_version(pipRepoPath, "pip", "head", "9.9.9-dev");

    init_git_repository(npmRepoPath);
    commit_plugin_source_version(npmRepoPath, "npm", "v1", "2.0.0", "v2.0.0");
    commit_plugin_source_version(npmRepoPath, "npm", "v2", "2.1.0", "v2.1.0");
    commit_plugin_source_version(npmRepoPath, "npm", "head", "9.9.9-dev");

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
        "    sources = {\n"
        "      pip = { source = 'git+" + pipRepoPath.string() + "' },\n"
        "      npm = { source = 'git+" + npmRepoPath.string() + "' },\n"
        "    },\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "}\n");

    {
        ReqPackConfig defaults = default_reqpack_config();
        defaults.registry.remoteUrl.clear();
        Registry registry(load_config_from_lua(configPath, defaults));
        REQUIRE(registry.getDatabase()->ensureReady());
        REQUIRE(registry.loadPlugin("pip"));
        REQUIRE(registry.loadPlugin("npm"));
        CHECK(registry.getPlugin("pip")->getVersion() == "9.9.9-dev");
        CHECK(registry.getPlugin("npm")->getVersion() == "9.9.9-dev");
    }

    const std::string output = run_reqpack(workspace, configPath, {"update", "--all"});
    INFO(output);
    CHECK(output.find("UPDATE:") != std::string::npos);
    CHECK(output.find("UPDATE done:  2 ok") != std::string::npos);
    const std::string refreshedPipScript = read_file(pluginDirectory / "pip" / "pip.lua");
    const std::string refreshedNpmScript = read_file(pluginDirectory / "npm" / "npm.lua");
    CAPTURE(refreshedPipScript);
    CAPTURE(refreshedNpmScript);
    CHECK(refreshedPipScript.find("1.2.0") != std::string::npos);
    CHECK(refreshedNpmScript.find("2.1.0") != std::string::npos);
}

TEST_CASE("update --all refreshes configured sources before plugins exist locally", "[integration][orchestrator][service][plugin-update]") {
    TempDir tempDir{"reqpack-plugin-wrapper-update-all-config-sources"};
    const std::filesystem::path workspace = tempDir.path() / "workspace";
    const std::filesystem::path pipRepoPath = tempDir.path() / "pip-origin";
    const std::filesystem::path npmRepoPath = tempDir.path() / "npm-origin";
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    std::filesystem::create_directories(workspace);

    init_git_repository(pipRepoPath);
    commit_plugin_source_version(pipRepoPath, "pip", "v1", "1.0.0", "v1.0.0");
    commit_plugin_source_version(pipRepoPath, "pip", "v2", "1.2.0", "v1.2.0");
    commit_plugin_source_version(pipRepoPath, "pip", "head", "9.9.9-dev");

    init_git_repository(npmRepoPath);
    commit_plugin_source_version(npmRepoPath, "npm", "v1", "2.0.0", "v2.0.0");
    commit_plugin_source_version(npmRepoPath, "npm", "v2", "2.1.0", "v2.1.0");
    commit_plugin_source_version(npmRepoPath, "npm", "head", "9.9.9-dev");

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
        "    sources = {\n"
        "      pip = { source = 'git+" + pipRepoPath.string() + "' },\n"
        "      npm = { source = 'git+" + npmRepoPath.string() + "' },\n"
        "    },\n"
        "  },\n"
        "  interaction = {\n"
        "    interactive = false,\n"
        "  },\n"
        "}\n");

    REQUIRE_FALSE(std::filesystem::exists(pluginDirectory / "pip" / "pip.lua"));
    REQUIRE_FALSE(std::filesystem::exists(pluginDirectory / "npm" / "npm.lua"));

    const std::string output = run_reqpack(workspace, configPath, {"update", "--all"});
    INFO(output);
    CHECK(output.find("UPDATE:") != std::string::npos);
    CHECK(output.find("UPDATE done:  2 ok") != std::string::npos);
    CHECK(read_file(pluginDirectory / "pip" / "pip.lua").find("1.2.0") != std::string::npos);
    CHECK(read_file(pluginDirectory / "npm" / "npm.lua").find("2.1.0") != std::string::npos);
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

TEST_CASE("orchestrator sbom resolves installed version for unversioned package request", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-installed-version"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path localPackage = build_rqp_package(
        tempDir.path(),
        "listed-artifact",
        "local out = context.paths.stateDir .. '/installed.txt'\ncontext.fs.mkdir(context.paths.stateDir)\ncontext.fs.copy(context.paths.payloadDir .. '/payload.txt', out)\nreturn true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello-list"))
    );
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    (void)run_reqpack(tempDir.path(), configPath, {"install", localPackage.string()});

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "sbom",
        "rqp",
        "listed-artifact",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    });

    CHECK(output.find(outputPath.string()) != std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string sbom = read_file(outputPath);
    CHECK(sbom.find("\"system\": \"rqp\"") != std::string::npos);
    CHECK(sbom.find("\"name\": \"listed-artifact\"") != std::string::npos);
    CHECK(sbom.find("\"version\": \"1.0.0-1+r0\"") != std::string::npos);
}

TEST_CASE("orchestrator sbom resolves unversioned package via info without list noise", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-info-only-version"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    add_plugin_script(pluginDirectory, "apply", SBOM_INFO_ONLY_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "sbom",
        "apply",
        "resolved",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    });

    CHECK(output.find("listed:") == std::string::npos);
    CHECK(output.find("informed:") == std::string::npos);
    CHECK(output.find("resolver-exec") == std::string::npos);
    CHECK(output.find("unexpected-list-call") == std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string sbom = read_file(outputPath);
    CHECK(sbom.find("\"name\": \"resolved\"") != std::string::npos);
    CHECK(sbom.find("\"version\": \"4.5.6\"") != std::string::npos);
}

TEST_CASE("orchestrator sbom fails by default when info cannot resolve version", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-unresolved-version"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    add_plugin_script(pluginDirectory, "apply", SBOM_INFO_ONLY_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "sbom",
        "apply",
        "missing",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    }, status);

    CHECK(output.find("listed:") == std::string::npos);
    CHECK(output.find("informed:") == std::string::npos);
    CHECK(output.find("resolver-exec") == std::string::npos);
    CHECK(status != 0);
    CHECK(output.find("sbom missing package: apply:missing") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(outputPath));
}

TEST_CASE("orchestrator sbom fails when requested package cannot be resolved", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-missing-fails"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    add_plugin_script(pluginDirectory, "apply", SBOM_RESOLVE_PLUGIN);

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "sbom",
        "apply",
        "missing",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    }, status);

    CHECK(status != 0);
    CHECK(output.find("sbom missing package: apply:missing") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(outputPath));
}

TEST_CASE("orchestrator sbom can skip missing package via cli flag", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-missing-skip-cli"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

    add_plugin_script(pluginDirectory, "apply", SBOM_RESOLVE_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "sbom",
        "apply",
        "resolved",
        "missing",
        "--sbom-skip-missing-packages",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    });

    CHECK(output.find("sbom skipping missing package: apply:missing") != std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string sbom = read_file(outputPath);
    CHECK(sbom.find("\"name\": \"resolved\"") != std::string::npos);
    CHECK(sbom.find("\"name\": \"missing\"") == std::string::npos);
}

TEST_CASE("orchestrator sbom can skip missing package via config", "[integration][orchestrator][service]") {
    TempDir tempDir{"reqpack-orchestrator-sbom-missing-skip-config"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = tempDir.path() / "config.lua";
    const std::filesystem::path outputPath = tempDir.path() / "graph.json";

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
        "  sbom = {\n"
        "    skipMissingPackages = true,\n"
        "  },\n"
        "  rqp = {\n"
        "    statePath = '" + (tempDir.path() / "rqp-state").string() + "',\n"
        "  },\n"
        "}\n");

    add_plugin_script(pluginDirectory, "apply", SBOM_RESOLVE_PLUGIN);

    const std::string output = run_reqpack(tempDir.path(), configPath, {
        "sbom",
        "apply",
        "resolved",
        "missing",
        "--format",
        "json",
        "--output",
        outputPath.string(),
    });

    CHECK(output.find("sbom skipping missing package: apply:missing") != std::string::npos);
    REQUIRE(std::filesystem::exists(outputPath));
    const std::string sbom = read_file(outputPath);
    CHECK(sbom.find("\"name\": \"resolved\"") != std::string::npos);
    CHECK(sbom.find("\"name\": \"missing\"") == std::string::npos);
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
    const std::string output = run_reqpack_with_home_env_and_status(tempDir.path(), configPath, tempDir.path(), {
        {"COLUMNS", "80"},
    }, {
        "audit",
        "apply",
    }, status);

    CHECK(status != 0);
    CHECK(output.find('\t') == std::string::npos);
    CHECK(output.find("SYSTEM NAME") != std::string::npos);
    CHECK(output.find("apply") != std::string::npos);
    CHECK(output.find("CVE-2026") != std::string::npos);
    CHECK(output.find("installed") != std::string::npos);
    CHECK(output.find("package issue") != std::string::npos);

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
    CHECK(output.find("LIST: apply") != std::string::npos);
    CHECK(output.find("1.0.0") != std::string::npos);
    CHECK(output.find("listed from") != std::string::npos);
    CHECK(output.find("apply") != std::string::npos);
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

TEST_CASE("reqpack install refuses prompted unsafe run in non-interactive mode", "[integration][orchestrator][security]") {
    TempDir tempDir{"reqpack-orchestrator-security-prompt-non-interactive"};
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
        "    onUnsafe = 'prompt',\n"
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
    CHECK(output.find("interactive mode is disabled; refusing to continue.") != std::string::npos);
    CHECK(output.find("execution blocked by security policy") != std::string::npos);
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
    CHECK(listResponse.second.find("apply") != std::string::npos);
    CHECK(listResponse.second.find("1.0.0") != std::string::npos);
    CHECK(listResponse.second.find("listed from") != std::string::npos);
    CHECK(listResponse.second.find("apply") != std::string::npos);
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

TEST_CASE("reqpack serve remote json mode rejects invalid json requests", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-json-invalid"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--json", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "not-json\n"));
    const std::string response = read_json_response_line(client);
    INFO(response);
    CHECK(response.find("\"ok\":false") != std::string::npos);
    CHECK(response.find("invalid json request") != std::string::npos);
    ::close(client);
}

TEST_CASE("reqpack serve remote json mode accepts empty commands after auth", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-json-empty"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);
    const std::filesystem::path logPath = tempDir.path() / "server.log";
    const int port = reserve_tcp_port();

    add_plugin_script(pluginDirectory, "apply", ORCHESTRATOR_PLUGIN);

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--json", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const int client = connect_with_retry("127.0.0.1", port);
    REQUIRE(send_socket_text(client, "{\"token\":\"secret\",\"command\":\"\"}\n"));
    const std::string response = read_json_response_line(client);
    CHECK(response.find("\"ok\":true") != std::string::npos);
    CHECK(response.find("\"output\":\"\"") != std::string::npos);
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

TEST_CASE("reqpack serve remote http and https modes report unimplemented", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-http"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    SECTION("http") {
        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(
            tempDir.path(),
            configPath,
            tempDir.path(),
            {"serve", "--remote", "--http"},
            status
        );
        CHECK(status != 0);
        CHECK(output.find("serve --remote --http is not implemented yet") != std::string::npos);
    }

    SECTION("https") {
        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(
            tempDir.path(),
            configPath,
            tempDir.path(),
            {"serve", "--remote", "--https"},
            status
        );
        CHECK(status != 0);
        CHECK(output.find("serve --remote --https is not implemented yet") != std::string::npos);
    }
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
    CHECK(countResponse.second.find("SERVE: connections") != std::string::npos);
    CHECK(countResponse.second.find("Active Connections") != std::string::npos);
    CHECK(countResponse.second.find("2") != std::string::npos);

    REQUIRE(send_socket_text(adminClient, "connections list\n"));
    const auto listResponse = read_text_protocol_response(adminClient);
    CHECK(listResponse.first == "OK");
    CHECK(listResponse.second.find("User") != std::string::npos);
    CHECK(listResponse.second.find("Admin") != std::string::npos);
    CHECK(listResponse.second.find("alice") != std::string::npos);
    CHECK(listResponse.second.find("root") != std::string::npos);
    CHECK(listResponse.second.find("true") != std::string::npos);

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
    CHECK(output.find("apply") != std::string::npos);
    CHECK(output.find("1.0.0") != std::string::npos);
    CHECK(output.find("listed from") != std::string::npos);
    CHECK(output.find("apply") != std::string::npos);
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

TEST_CASE("reqpack remote reports missing named profile", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-missing-profile"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = { host = '127.0.0.1', port = 4545 },\n"
        "}\n");

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "remote", "missing", "list", "apply"
    }, status);
    CHECK(status != 0);
    CHECK(output.find("remote profile not found: missing") != std::string::npos);
}

TEST_CASE("reqpack remote json profiles require forwarded command", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-json-no-command"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    host = '127.0.0.1',\n"
        "    port = 4545,\n"
        "    protocol = 'json',\n"
        "  },\n"
        "}\n");

    int status = 0;
    const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
        "remote", "dev"
    }, status);
    CHECK(status != 0);
    CHECK(output.find("json remote profiles require a forwarded command") != std::string::npos);
}

TEST_CASE("reqpack remote rejects invalid local upload arguments before connecting", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-upload-errors"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    write_remote_profiles(tempDir.path(),
        "return {\n"
        "  dev = {\n"
        "    host = '127.0.0.1',\n"
        "    port = 4545,\n"
        "  },\n"
        "}\n");

    SECTION("non-regular upload path is rejected") {
        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
            "remote", "dev", "install", "apply", tempDir.path().string()
        }, status);
        CHECK(status != 0);
        CHECK(output.find("remote upload only supports regular files") != std::string::npos);
    }

    SECTION("multiple local upload files are rejected") {
        const std::filesystem::path firstUpload = tempDir.path() / "one.pkg";
        const std::filesystem::path secondUpload = tempDir.path() / "two.pkg";
        write_file(firstUpload, "one");
        write_file(secondUpload, "two");

        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
            "remote", "dev", "install", "apply", firstUpload.string(), secondUpload.string()
        }, status);
        CHECK(status != 0);
        CHECK(output.find("remote upload supports one local file per install command") != std::string::npos);
    }
}

TEST_CASE("reqpack remote interactive client reports local input errors through diagnostics", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-client-input-errors"};
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
        "    token = 'secret',\n"
        "  },\n"
        "}\n");

    ServerProcess server(tempDir.path(), configPath, std::nullopt, {
        "serve", "--remote", "--bind", "127.0.0.1", "--port", std::to_string(port), "--token", "secret"
    }, logPath);

    const std::filesystem::path badUploadDir = tempDir.path() / "upload-dir";
    std::filesystem::create_directories(badUploadDir);
    const std::string output = run_reqpack_with_home_and_stdin(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {"remote", "dev"},
        "\"unterminated\ninstall apply " + badUploadDir.string() + "\nquit\n"
    );

    CHECK(output.find("invalid command syntax") != std::string::npos);
    CHECK(output.find("Cause: Interactive remote command could not be tokenized.") != std::string::npos);
    CHECK(output.find("remote upload only supports regular files") != std::string::npos);
    CHECK(output.find("Cause: Interactive remote upload request is invalid.") != std::string::npos);
}

TEST_CASE("reqpack remote client reports http and https profiles as unimplemented", "[integration][orchestrator][remote]") {
    TempDir tempDir{"reqpack-orchestrator-remote-http-client"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config(tempDir.path(), pluginDirectory);

    SECTION("http profile") {
        write_remote_profiles(tempDir.path(),
            "return {\n"
            "  dev = { url = 'http://127.0.0.1:4545' },\n"
            "}\n");

        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
            "remote", "dev", "list", "apply"
        }, status);
        CHECK(status != 0);
        CHECK(output.find("http remote profiles are not implemented yet") != std::string::npos);
    }

    SECTION("https profile") {
        write_remote_profiles(tempDir.path(),
            "return {\n"
            "  dev = { url = 'https://127.0.0.1:4545' },\n"
            "}\n");

        int status = 0;
        const std::string output = run_reqpack_with_home_and_status(tempDir.path(), configPath, tempDir.path(), {
            "remote", "dev", "list", "apply"
        }, status);
        CHECK(status != 0);
        CHECK(output.find("https remote profiles are not implemented yet") != std::string::npos);
    }
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

TEST_CASE("orchestrator install maven uses configured repositories and auth settings", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-maven-repositories"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_maven_repositories(
        tempDir.path(),
        pluginDirectory,
        "      {\n"
        "        id = 'corp',\n"
        "        url = 'https://repo.example.test/maven-public',\n"
        "        priority = 1,\n"
        "        auth = {\n"
        "          type = 'token',\n"
        "          token = '${REQPACK_TEST_NEXUS_TOKEN}',\n"
        "          headerName = 'X-Repo-Token',\n"
        "        },\n"
        "        validation = {\n"
        "          checksum = 'fail',\n"
        "          tlsVerify = false,\n"
        "        },\n"
        "        scope = {\n"
        "          include = { 'org.junit:*' },\n"
        "        },\n"
        "        snapshots = false,\n"
        "        releases = true,\n"
        "      },\n"
        "      {\n"
        "        id = 'fallback',\n"
        "        url = 'https://repo.example.test/fallback',\n"
        "        priority = 50,\n"
        "      },\n"
    );
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path mvnLog = tempDir.path() / "mvn.log";
    const std::filesystem::path settingsCopy = tempDir.path() / "captured-settings.xml";
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
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(mvnLog.string()) + "\n"
        "settings=''\n"
        "prev=''\n"
        "for arg in \"$@\"; do\n"
        "  if [ \"$prev\" = '-s' ]; then settings=\"$arg\"; break; fi\n"
        "  prev=\"$arg\"\n"
        "done\n"
        "if [ -n \"$settings\" ]; then cp \"$settings\" " + escape_shell_arg(settingsCopy.string()) + "; fi\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "java").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "javac").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "mvn").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
            {"REQPACK_TEST_NEXUS_TOKEN", "secret-token"},
            {"REQPACK_MAVEN_REPO", (tempDir.path() / "custom-m2").string()},
        },
        {"install", "maven", "org.junit:junit:4.13"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("[error]") == std::string::npos);
    const std::string mavenInstallLog = read_file(mvnLog);
    CHECK(mavenInstallLog.find("-Dmaven.repo.local=" + (tempDir.path() / "custom-m2").string()) != std::string::npos);
    CHECK(mavenInstallLog.find("-DremoteRepositories=corp::default::https://repo.example.test/maven-public,fallback::default::https://repo.example.test/fallback") != std::string::npos);
    CHECK(mavenInstallLog.find("-Dmaven.wagon.http.ssl.insecure=true") != std::string::npos);
    CHECK(mavenInstallLog.find("-Dmaven.wagon.http.ssl.allowall=true") != std::string::npos);
    CHECK(mavenInstallLog.find("-s ") != std::string::npos);
    CHECK(mavenInstallLog.find("dependency:get") != std::string::npos);

    REQUIRE(std::filesystem::exists(settingsCopy));
    const std::string settings = read_file(settingsCopy);
    CHECK(settings.find("<id>corp</id>") != std::string::npos);
    CHECK(settings.find("<url>https://repo.example.test/maven-public</url>") != std::string::npos);
    CHECK(settings.find("<name>X-Repo-Token</name>") != std::string::npos);
    CHECK(settings.find("<value>secret-token</value>") != std::string::npos);
    CHECK(settings.find("<checksumPolicy>fail</checksumPolicy>") != std::string::npos);
    CHECK(settings.find("<enabled>false</enabled>") != std::string::npos);
}

TEST_CASE("orchestrator install maven fails when configured repositories do not match scope", "[integration][orchestrator][service][sys]") {
    TempDir tempDir{"reqpack-orchestrator-maven-scope-miss"};
    const std::filesystem::path pluginDirectory = tempDir.path() / "plugins";
    const std::filesystem::path configPath = write_config_with_maven_repositories(
        tempDir.path(),
        pluginDirectory,
        "      {\n"
        "        id = 'corp',\n"
        "        url = 'https://repo.example.test/maven-public',\n"
        "        priority = 1,\n"
        "        scope = {\n"
        "          include = { 'com.mycompany.*' },\n"
        "        },\n"
        "      },\n"
    );
    const std::filesystem::path fakeBin = tempDir.path() / "bin";
    const std::filesystem::path mvnLog = tempDir.path() / "mvn.log";
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
        "printf '%s\n' \"$*\" >> " + escape_shell_arg(mvnLog.string()) + "\n"
        "exit 0\n");
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "java").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "javac").string())).c_str()) == 0);
    REQUIRE(std::system(("chmod +x " + escape_shell_arg((fakeBin / "mvn").string())).c_str()) == 0);

    const char* currentPath = std::getenv("PATH");
    const std::string pathValue = fakeBin.string() + ":" + (currentPath != nullptr ? currentPath : "");

    int status = 0;
    const std::string output = run_reqpack_with_home_env_and_status(
        tempDir.path(),
        configPath,
        tempDir.path(),
        {
            {"PATH", pathValue},
        },
        {"install", "maven", "org.junit:junit:4.13"},
        status
    );

    CHECK(status == 0);
    CHECK(output.find("no configured maven repository matched org.junit:junit") != std::string::npos);
    CHECK(output.find("INSTALL done:  0 ok,  0 skipped,  1 failed") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(mvnLog));
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
