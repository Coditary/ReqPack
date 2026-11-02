#include "main_live.h"

#include "cli/cli.h"
#include <reqpack/process_runner.h>
#include <reqpack/orchestrator.h>
#include <reqpack/plugin_bundle.h>

#include <reqpack/config.h>

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace {

#ifndef REQPACK_SOURCE_DIR
#define REQPACK_SOURCE_DIR "."
#endif

struct LiveMatrixRow {
    std::string id;
    std::string image;
    std::string system;
    std::string pluginRelpath;
    std::string package;
    std::string verifyCmd;
    std::string mode;
    std::string profileEnv;
    bool enabled {false};
};

struct LiveRunOptions {
    std::filesystem::path repoRoot;
    std::filesystem::path matrixFile;
    std::filesystem::path workRoot;
    std::optional<std::filesystem::path> bundleArchive;
    bool skipBuild {false};
    bool skipImages {false};
    bool prepareOnly {false};
    bool quick {false};
    std::vector<std::string> filterIds;
    std::optional<std::filesystem::path> pluginPackagePath;
    bool skipPack {false};
    bool ephemeralWorkRoot {false};
    bool keepWorkRoot {false};
};

void live_log(Logger& /*logger*/, const std::string& message) {
    std::cerr << "[live-test] " << message << '\n';
}

class EphemeralWorkRootCleanup {
public:
    EphemeralWorkRootCleanup(Logger& logger, const LiveRunOptions& options)
        : logger_(logger), path_(options.workRoot), enabled_(should_remove(options)) {}

    ~EphemeralWorkRootCleanup() {
        if (!enabled_ || path_.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        if (ec) {
            live_log(logger_, "failed to remove ephemeral work root " + path_.string() + ": " + ec.message());
            return;
        }
        live_log(logger_, "removed ephemeral work root " + path_.string());
    }

private:
    static bool should_remove(const LiveRunOptions& options) {
        if (!options.ephemeralWorkRoot || options.keepWorkRoot || options.prepareOnly) {
            return false;
        }
        return true;
    }

    Logger& logger_;
    std::filesystem::path path_;
    bool enabled_;
};

std::string trim_copy(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split_tsv_field(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (char ch : line) {
        if (ch == '|') {
            fields.push_back(trim_copy(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(trim_copy(current));
    return fields;
}

std::optional<std::string> environment_value(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
        return std::string {value};
    }
    return std::nullopt;
}

std::filesystem::path default_repo_root() {
    if (const auto env = environment_value("REQPACK_REPO_ROOT")) {
        return std::filesystem::absolute(*env);
    }
    const std::filesystem::path compiled {REQPACK_SOURCE_DIR};
    if (std::filesystem::is_directory(compiled)) {
        return std::filesystem::absolute(compiled);
    }
    return std::filesystem::absolute(std::filesystem::current_path());
}

std::filesystem::path plugins_root(const std::filesystem::path& repoRoot) {
    if (const auto env = environment_value("REQPACK_PLUGINS_ROOT")) {
        return std::filesystem::absolute(*env);
    }
    const std::filesystem::path sibling = repoRoot.parent_path() / "reqpack-plugins";
    if (std::filesystem::is_directory(sibling)) {
        return sibling;
    }
    const std::filesystem::path nested = repoRoot / "reqpack-plugins";
    if (std::filesystem::is_directory(nested)) {
        return nested;
    }
    return sibling;
}

bool selinux_enabled() {
    const ReqpackProcessResult result =
        reqpack_run_process_capture({"bash", "-lc", "command -v selinuxenabled >/dev/null && selinuxenabled"});
    return result.success();
}

std::string volume_option(const std::filesystem::path& hostPath, const std::string& containerPath, bool writable) {
    std::string mount = hostPath.lexically_normal().string() + ":" + containerPath + (writable ? ":rw" : ":ro");
    if (selinux_enabled()) {
        mount += writable ? ",z" : ",Z";
    }
    return mount;
}

void append_volume_mount(std::vector<std::string>& runArgs,
                         const std::filesystem::path& hostPath,
                         const std::string& containerPath,
                         bool writable) {
    runArgs.push_back("-v");
    runArgs.push_back(volume_option(hostPath, containerPath, writable));
}

struct ContainerEngine {
    std::string binary;
    std::string name;
};

std::optional<ContainerEngine> detect_container_engine() {
    if (const auto forced = environment_value("CONTAINER_BIN")) {
        if (*forced == "podman" || *forced == "docker") {
            return ContainerEngine {*forced, *forced};
        }
        return std::nullopt;
    }
    if (reqpack_run_process_capture({"bash", "-lc", "command -v podman >/dev/null"}).success()) {
        return ContainerEngine {"podman", "podman"};
    }
    if (reqpack_run_process_capture({"bash", "-lc", "command -v docker >/dev/null"}).success()) {
        return ContainerEngine {"docker", "docker"};
    }
    return std::nullopt;
}

std::string shell_single_quote(const std::string& value) {
    std::string escaped = "'";
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

void write_live_config(const std::filesystem::path& workspaceDir, const std::filesystem::path& pluginDir) {
    const std::filesystem::path configPath = workspaceDir / "config.lua";
    std::filesystem::create_directories(workspaceDir);
    std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
    output << "return {\n"
           << "  security = {\n"
           << "    autoFetch = false,\n"
           << "    enabled = true,\n"
           << "    osvFeedUrl = \"/workspace/live-fixtures/security/conformance-osv-feed.json\",\n"
           << "    osvDatabasePath = \"" << (workspaceDir / "osv-db").string() << "\",\n"
           << "    osvRefreshMode = \"always\",\n"
           << "    osvEcosystemMap = {\n"
           << "      dpkg = \"Debian\",\n"
           << "      nix = \"Nix\",\n"
           << "    },\n"
           << "  },\n"
           << "  execution = {\n"
           << "    useTransactionDb = false,\n"
           << "    deleteCommittedTransactions = false,\n"
           << "    checkVirtualFileSystemWrite = false,\n"
           << "    transactionDatabasePath = \"" << (workspaceDir / "transactions").string() << "\",\n"
           << "  },\n"
           << "  planner = {\n"
           << "    autoDownloadMissingPlugins = false,\n"
           << "    autoDownloadMissingDependencies = false,\n"
           << "  },\n"
           << "  registry = {\n"
           << "    pluginDirectory = \"" << pluginDir.string() << "\",\n"
           << "    databasePath = \"" << (workspaceDir / "registry-db").string() << "\",\n"
           << "    autoLoadPlugins = true,\n"
           << "    shutDownPluginsOnExit = true,\n"
           << "  },\n"
           << "  interaction = {\n"
           << "    interactive = false,\n"
           << "  },\n"
           << "  rqp = {\n"
           << "    statePath = \"" << (workspaceDir / "rqp-state").string() << "\",\n"
           << "  },\n"
           << "}\n";
}

int run_orchestrator_tokens(ReqPackConfig& config, const std::vector<std::string>& tokens) {
    Cli cli;
    const std::vector<Request> requests = cli.parse(tokens, config);
    if (requests.empty()) {
        return 1;
    }
    const bool previousInteractive = config.interaction.interactive;
    config.interaction.interactive = false;
    Orchestrator orchestrator(requests, config);
    const int exitCode = orchestrator.run();
    config.interaction.interactive = previousInteractive;
    return exitCode;
}

std::filesystem::path image_context_for_tag(const std::filesystem::path& liveRoot, const std::string& tag) {
    const std::string suffix = tag.rfind(':') == std::string::npos ? tag : tag.substr(tag.rfind(':') + 1);
    if (suffix == "debian-bookworm") {
        return liveRoot / "images/debian-bookworm";
    }
    if (suffix == "fedora-43") {
        return liveRoot / "images/fedora-43";
    }
    if (suffix == "archlinux") {
        return liveRoot / "images/archlinux";
    }
    if (suffix == "node-22-bookworm") {
        return liveRoot / "images/node-22-bookworm";
    }
    if (suffix == "nix") {
        return liveRoot / "images/nix";
    }
    if (suffix == "nix-bootstrap") {
        return liveRoot / "images/nix-bootstrap";
    }
    throw std::runtime_error("unknown live image tag: " + tag);
}

std::string resolve_live_image(const std::string& tag) {
    if (const auto registry = environment_value("LIVE_IMAGE_REGISTRY")) {
        const std::string suffix = tag.rfind(':') == std::string::npos ? tag : tag.substr(tag.rfind(':') + 1);
        return *registry + "/reqpack-live-test:" + suffix;
    }
    return tag;
}

bool extract_linux_bundle(const std::filesystem::path& archive, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination);
    return reqpack_run_process({"tar", "-xzf", archive.string(), "-C", destination.string()}, destination.parent_path());
}

bool prepare_bundle(const LiveRunOptions& options,
                    Logger& logger,
                    const std::filesystem::path& bundleDir,
                    const std::optional<ContainerEngine>& engine) {
    const std::filesystem::path readyMarker = options.workRoot / "bundle.ready";
    if (options.skipBuild && std::filesystem::exists(readyMarker)) {
        live_log(logger, "reusing prepared bundle at " + bundleDir.string());
        return true;
    }

    if (options.bundleArchive.has_value()) {
        if (!std::filesystem::is_regular_file(*options.bundleArchive)) {
            live_log(logger, "bundle missing: " + options.bundleArchive->string());
            return false;
        }
        if (!extract_linux_bundle(*options.bundleArchive, bundleDir)) {
            return false;
        }
        std::ofstream(readyMarker).put('\n');
        return true;
    }

    if (options.skipBuild && !std::filesystem::exists(readyMarker) && !options.bundleArchive.has_value()) {
        live_log(logger, "--skip-build requires --bundle or an existing prepared bundle (bundle.ready in work root)");
        return false;
    }

    const bool hostBuild = environment_value("REQPACK_LIVE_HOST_BUILD") == "1";
    if (!hostBuild && engine.has_value()) {
        const std::filesystem::path buildScript = options.repoRoot / "scripts/build_live_bundle_in_container.sh";
        if (!std::filesystem::is_regular_file(buildScript)) {
            live_log(logger, "missing container bundle script: " + buildScript.string());
            return false;
        }
        const std::filesystem::path monorepoRoot = options.repoRoot.parent_path();
        live_log(logger,
                 "building rqp bundle in debian bookworm container (set REQPACK_LIVE_HOST_BUILD=1 to build on host)");
        if (!reqpack_run_process({buildScript.string(),
                                  engine->binary,
                                  monorepoRoot.string(),
                                  options.workRoot.string(),
                                  options.repoRoot.string()},
                                 options.repoRoot)) {
            return false;
        }
        const std::filesystem::path bundleArchive = options.workRoot / "rqp-linux.tar.gz";
        if (!std::filesystem::is_regular_file(bundleArchive)) {
            live_log(logger, "container build did not produce: " + bundleArchive.string());
            return false;
        }
        if (!extract_linux_bundle(bundleArchive, bundleDir)) {
            return false;
        }
        std::ofstream(readyMarker).put('\n');
        return true;
    }

    const std::filesystem::path localBuildDir = options.workRoot / "build";
    std::filesystem::create_directories(localBuildDir);
    live_log(logger, "building rqp bundle under " + localBuildDir.string());
    if (!reqpack_run_process({"cmake", "-S", options.repoRoot.string(), "-B", localBuildDir.string(),
                              "-DCMAKE_BUILD_TYPE=Release"},
                             options.repoRoot)) {
        return false;
    }
    if (!reqpack_run_process({"cmake", "--build", localBuildDir.string(), "--parallel", "--target", "ReqPack"},
                             options.repoRoot)) {
        return false;
    }
    const std::filesystem::path bundleArchive = options.workRoot / "rqp-linux.tar.gz";
    const std::filesystem::path packageScript = options.repoRoot / "scripts/package_release_bundle.sh";
    if (!reqpack_run_process({packageScript.string(), (localBuildDir / "rqp").string(), bundleArchive.string(), "linux"},
                             options.repoRoot)) {
        return false;
    }
    if (!extract_linux_bundle(bundleArchive, bundleDir)) {
        return false;
    }
    std::ofstream(readyMarker).put('\n');
    return true;
}

bool ensure_live_images(const LiveRunOptions& options, Logger& logger, const ContainerEngine& engine) {
    if (options.skipImages) {
        return true;
    }
    const std::filesystem::path liveRoot = options.repoRoot / "tests/system/live";
    const std::vector<std::string> tags {
        "reqpack-live-test:debian-bookworm",
        "reqpack-live-test:fedora-43",
        "reqpack-live-test:archlinux",
        "reqpack-live-test:node-22-bookworm",
        "reqpack-live-test:nix",
        "reqpack-live-test:nix-bootstrap",
    };
    for (const std::string& tag : tags) {
        const std::filesystem::path context = image_context_for_tag(liveRoot, tag);
        live_log(logger, "building cached base image " + tag);
        if (!reqpack_run_process({engine.binary, "build", "-t", tag, "-f", (context / "Containerfile").string(),
                                  context.string()},
                                 options.repoRoot)) {
            return false;
        }
    }
    std::ofstream(options.workRoot / "images.ready").put('\n');
    return true;
}

std::vector<LiveMatrixRow> load_matrix(const std::filesystem::path& matrixFile) {
    std::ifstream input(matrixFile);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open matrix file: " + matrixFile.string());
    }
    std::vector<LiveMatrixRow> rows;
    std::string line;
    while (std::getline(input, line)) {
        line = trim_copy(line);
        if (line.empty() || line.rfind('#', 0) == 0) {
            continue;
        }
        const std::vector<std::string> fields = split_tsv_field(line);
        if (fields.size() < 8) {
            continue;
        }
        LiveMatrixRow row;
        row.id = fields[0];
        row.image = fields[1];
        row.system = fields[2];
        row.pluginRelpath = fields[3];
        row.package = fields[4];
        row.verifyCmd = fields[5];
        row.mode = fields[6].empty() ? "repo" : fields[6];
        row.enabled = fields[7] == "1";
        row.profileEnv = fields.size() >= 9 && !fields[8].empty() ? fields[8] : (row.system + ".env");
        rows.push_back(std::move(row));
    }
    return rows;
}

bool row_selected(const LiveRunOptions& options, const std::string& id) {
    if (options.quick && id != "debian-dpkg") {
        return false;
    }
    if (options.filterIds.empty()) {
        return true;
    }
    for (const std::string& wanted : options.filterIds) {
        if (wanted == id) {
            return true;
        }
    }
    return false;
}

int run_named_container(Logger& logger,
                        const ContainerEngine& engine,
                        const std::string& containerName,
                        const std::vector<std::string>& arguments) {
    reqpack_run_process({engine.binary, "rm", "-f", containerName});
    std::vector<std::string> command {engine.binary, "run", "--replace", "--name", containerName, "--rm"};
    if (selinux_enabled()) {
        command.push_back("--security-opt");
        command.push_back("label=disable");
    }
    command.insert(command.end(), arguments.begin(), arguments.end());
    const ReqpackProcessResult captured = reqpack_run_process_capture(command);
    if (captured.exitCode != 0) {
        if (!captured.stderrText.empty()) {
            live_log(logger, engine.name + ": " + captured.stderrText);
        } else if (!captured.stdoutText.empty()) {
            live_log(logger, engine.name + ": " + captured.stdoutText);
        }
    }
    reqpack_run_process({engine.binary, "rm", "-f", containerName});
    return captured.exitCode;
}

bool report_indicates_success(const std::filesystem::path& reportPath) {
    std::ifstream input(reportPath);
    if (!input.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();
    return content.find("\"failed\": 0") != std::string::npos ||
           content.find("\"failed\":0") != std::string::npos;
}

int run_plugin_live_row(const LiveRunOptions& options,
                        Logger& logger,
                        ReqPackConfig& config,
                        const ContainerEngine& engine,
                        const LiveMatrixRow& row,
                        const std::filesystem::path& bundleDir,
                        const std::filesystem::path& fixturesRoot) {
    const std::filesystem::path pluginsRoot = plugins_root(options.repoRoot);
    const std::filesystem::path pluginSource = pluginsRoot / row.pluginRelpath;
    if (!std::filesystem::is_directory(pluginSource)) {
        live_log(logger, "missing plugin source: " + pluginSource.string());
        return 1;
    }

    const std::optional<PluginBundleProbe> probe = plugin_bundle_probe_directory(pluginSource);
    if (!probe.has_value()) {
        live_log(logger, "plugin source is not a valid bundle: " + pluginSource.string());
        return 1;
    }
    const std::string pluginId = probe->metadata.name;

    const std::filesystem::path rowDir = options.workRoot / "rows" / row.id;
    const std::filesystem::path workspaceDir = rowDir / "workspace";
    const std::filesystem::path pluginDir = workspaceDir / "plugins" / row.system;
    const std::filesystem::path artifactsDir = workspaceDir / "artifacts";
    const std::filesystem::path casesDir = workspaceDir / "plugin-tests/live";
    const std::filesystem::path reportPath = workspaceDir / "reports/plugin-live.json";
    std::filesystem::create_directories(artifactsDir);
    std::filesystem::create_directories(casesDir.parent_path());
    std::filesystem::create_directories(reportPath.parent_path());

    const std::filesystem::path liveCases = pluginSource / ".reqpack-test/live";
    if (!std::filesystem::is_directory(liveCases)) {
        live_log(logger, "missing live preset directory: " + liveCases.string());
        return 1;
    }
    std::filesystem::copy(liveCases,
                          casesDir,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);

    const std::filesystem::path packagePath = artifactsDir / (pluginId + ".rqp");
    if (options.pluginPackagePath.has_value()) {
        std::filesystem::copy(*options.pluginPackagePath,
                              packagePath,
                              std::filesystem::copy_options::overwrite_existing);
    } else if (!options.skipPack) {
        live_log(logger, "packing plugin " + pluginId + " from " + pluginSource.string());
        const int packStatus =
            run_orchestrator_tokens(config,
                                    {"pack", pluginSource.string(), "--output", packagePath.string(), "--force"});
        if (packStatus != 0 || !std::filesystem::is_regular_file(packagePath)) {
            live_log(logger, "failed to pack plugin artifact: " + packagePath.string());
            return 1;
        }
    } else if (!std::filesystem::is_regular_file(packagePath)) {
        live_log(logger, "plugin package missing and packing disabled: " + packagePath.string());
        return 1;
    }

    write_live_config(workspaceDir, workspaceDir / "plugins");

    const std::string containerImage = resolve_live_image(row.image);
    const std::string runId = environment_value("LIVE_RUN_ID").value_or(options.workRoot.filename().string());
    const std::string containerName = "rqp-live-" + runId + "-" + row.id;

    std::ostringstream inner;
    inner << "export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua; "
          << "/opt/reqpack/rqp --config /workspace/config.lua -y install rqp "
          << shell_single_quote("/workspace/artifacts/" + pluginId + ".rqp") << " && "
          << "/opt/reqpack/rqp --config /workspace/config.lua -y test-plugin --plugin "
          << shell_single_quote(pluginId) << " --live --cases "
          << shell_single_quote("/workspace/plugin-tests/live") << " --report "
          << shell_single_quote("/workspace/reports/plugin-live.json");

    std::vector<std::string> runArgs;
    append_volume_mount(runArgs, bundleDir, "/opt/reqpack", false);
    append_volume_mount(runArgs, workspaceDir, "/workspace", true);
    append_volume_mount(runArgs, fixturesRoot, "/workspace/live-fixtures", false);
    runArgs.push_back(containerImage);
    runArgs.push_back("bash");
    runArgs.push_back("-lc");
    runArgs.push_back(inner.str());

    live_log(logger, "row " + row.id + ": " + containerImage + " system=" + row.system + " mode=plugin-live");
    const int status = run_named_container(logger, engine, containerName, runArgs);
    if (status != 0) {
        live_log(logger, "FAIL " + row.id + " (exit " + std::to_string(status) + ")");
        return status;
    }
    if (!report_indicates_success(reportPath)) {
        live_log(logger, "FAIL " + row.id + " (plugin test report unsuccessful)");
        return 1;
    }
    live_log(logger, "PASS " + row.id);
    return 0;
}

int run_conformance_row(const LiveRunOptions& options,
                        Logger& logger,
                        ReqPackConfig& config,
                        const ContainerEngine& engine,
                        const LiveMatrixRow& row,
                        const std::filesystem::path& bundleDir,
                        const std::filesystem::path& fixturesRoot,
                        const std::filesystem::path& liveRoot) {
    const std::filesystem::path pluginsRoot = plugins_root(options.repoRoot);
    const std::filesystem::path pluginSource = pluginsRoot / row.pluginRelpath;
    if (!std::filesystem::is_directory(pluginSource)) {
        live_log(logger, "missing plugin source: " + pluginSource.string());
        return 1;
    }

    const std::optional<PluginBundleProbe> probe = plugin_bundle_probe_directory(pluginSource);
    if (!probe.has_value()) {
        live_log(logger, "plugin source is not a valid bundle: " + pluginSource.string());
        return 1;
    }
    const std::string pluginId = probe->metadata.name;

    const std::filesystem::path rowDir = options.workRoot / "rows" / row.id;
    const std::filesystem::path workspaceDir = rowDir / "workspace";
    const std::filesystem::path artifactsDir = workspaceDir / "artifacts";
    const std::filesystem::path conformanceReport = workspaceDir / "reports/conformance.json";
    const std::filesystem::path profilePath =
        liveRoot / "profiles" / row.profileEnv;
    if (!std::filesystem::is_regular_file(profilePath)) {
        live_log(logger, "missing conformance profile: " + profilePath.string());
        return 1;
    }

    std::filesystem::create_directories(artifactsDir);
    std::filesystem::create_directories(conformanceReport.parent_path());

    const std::filesystem::path packagePath = artifactsDir / (pluginId + ".rqp");
    if (options.pluginPackagePath.has_value()) {
        std::filesystem::copy(*options.pluginPackagePath,
                              packagePath,
                              std::filesystem::copy_options::overwrite_existing);
    } else if (!options.skipPack) {
        live_log(logger, "packing plugin " + pluginId + " from " + pluginSource.string());
        const int packStatus =
            run_orchestrator_tokens(config,
                                    {"pack", pluginSource.string(), "--output", packagePath.string(), "--force"});
        if (packStatus != 0 || !std::filesystem::is_regular_file(packagePath)) {
            live_log(logger, "failed to pack plugin artifact: " + packagePath.string());
            return 1;
        }
    } else if (!std::filesystem::is_regular_file(packagePath)) {
        live_log(logger, "plugin package missing and packing disabled: " + packagePath.string());
        return 1;
    }

    write_live_config(workspaceDir, workspaceDir / "plugins");

    const std::filesystem::path conformanceDir = workspaceDir / "conformance";
    std::filesystem::create_directories(conformanceDir / "lib");
    std::filesystem::create_directories(conformanceDir / "profiles");
    const auto copy_conformance_file = [](const std::filesystem::path& from, const std::filesystem::path& to) {
        std::filesystem::copy(from, to, std::filesystem::copy_options::overwrite_existing);
    };
    copy_conformance_file(liveRoot / "run_live_conformance.sh", conformanceDir / "run_live_conformance.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p1.sh", conformanceDir / "run_live_conformance_p1.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p2.sh", conformanceDir / "run_live_conformance_p2.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p3.sh", conformanceDir / "run_live_conformance_p3.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p4.sh", conformanceDir / "run_live_conformance_p4.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p5.sh", conformanceDir / "run_live_conformance_p5.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p6.sh", conformanceDir / "run_live_conformance_p6.sh");
    copy_conformance_file(liveRoot / "run_live_conformance_p7.sh", conformanceDir / "run_live_conformance_p7.sh");
    copy_conformance_file(liveRoot / "lib/conformance_common.sh", conformanceDir / "lib/conformance_common.sh");
    copy_conformance_file(profilePath, conformanceDir / "profiles" / row.profileEnv);

    const std::string containerImage = resolve_live_image(row.image);
    const std::string runId = environment_value("LIVE_RUN_ID").value_or(options.workRoot.filename().string());
    const std::string containerName = "rqp-live-" + runId + "-" + row.id;

    std::ostringstream inner;
    inner << "export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua "
          << "CONF_PLUGIN_DIR=/workspace/plugins; "
          << "bash /workspace/conformance/run_live_conformance.sh "
          << shell_single_quote("/workspace/conformance/profiles/" + row.profileEnv);

    std::vector<std::string> runArgs;
    append_volume_mount(runArgs, bundleDir, "/opt/reqpack", false);
    append_volume_mount(runArgs, workspaceDir, "/workspace", true);
    append_volume_mount(runArgs, fixturesRoot, "/workspace/live-fixtures", false);
    runArgs.push_back(containerImage);
    runArgs.push_back("bash");
    runArgs.push_back("-lc");
    runArgs.push_back(inner.str());

    live_log(logger,
             "row " + row.id + ": " + containerImage + " system=" + row.system + " mode=conformance");
    const int status = run_named_container(logger, engine, containerName, runArgs);
    if (status != 0) {
        live_log(logger, "FAIL " + row.id + " (exit " + std::to_string(status) + ")");
        return status;
    }
    if (!report_indicates_success(conformanceReport)) {
        live_log(logger, "FAIL " + row.id + " (conformance report unsuccessful)");
        return 1;
    }
    live_log(logger, "PASS " + row.id);
    return 0;
}

int run_repo_or_local_row(const LiveRunOptions& options,
                          Logger& logger,
                          const ContainerEngine& engine,
                          const LiveMatrixRow& row,
                          const std::filesystem::path& bundleDir,
                          const std::filesystem::path& fixturesRoot,
                          const std::filesystem::path& liveRoot) {
    const std::filesystem::path pluginsRoot = plugins_root(options.repoRoot);
    const std::filesystem::path rowDir = options.workRoot / "rows" / row.id;
    const std::filesystem::path workspaceDir = rowDir / "workspace";
    const std::filesystem::path pluginDir = workspaceDir / "plugins" / row.system;
    const std::filesystem::path scenarioDir = rowDir / "scenario";
    std::filesystem::create_directories(scenarioDir);
    std::filesystem::create_directories(workspaceDir / "reports");

    const std::filesystem::path pluginSource = pluginsRoot / row.pluginRelpath;
    std::filesystem::remove_all(pluginDir);
    std::filesystem::create_directories(pluginDir.parent_path());
    std::filesystem::copy(pluginSource, pluginDir, std::filesystem::copy_options::recursive);
    write_live_config(workspaceDir, workspaceDir / "plugins");

    std::string inner;
    if (row.mode == "repo") {
        inner = "export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua LIVE_REPORT_DIR=/workspace/reports; "
                "bash /workspace/test/run_live_lifecycle.sh " +
                shell_single_quote(row.system) + " " + shell_single_quote(row.package) + " " +
                shell_single_quote(row.verifyCmd);
    } else if (row.mode == "local") {
        const std::string fixtureRel = row.package.substr(0, row.package.find(':'));
        std::string removeName = row.package.substr(row.package.find(':') + 1);
        if (removeName == row.package) {
            removeName = "reqpack-live-fixture";
        }
        inner = "export RQP_BIN=/opt/reqpack/rqp LIVE_CONFIG=/workspace/config.lua; "
                "bash /workspace/test/run_live_local_install.sh " +
                shell_single_quote(row.system) + " " +
                shell_single_quote("/workspace/live-fixtures/" + fixtureRel) + " " +
                shell_single_quote(row.verifyCmd) + " " + shell_single_quote(removeName);
    } else {
        live_log(logger, "unknown live test mode: " + row.mode);
        return 1;
    }

    const std::filesystem::path scriptSource =
        row.mode == "repo" ? liveRoot / "run_live_lifecycle.sh" : liveRoot / "run_live_local_install.sh";
    const std::filesystem::path scriptDest = scenarioDir / scriptSource.filename();
    std::filesystem::copy(scriptSource, scriptDest, std::filesystem::copy_options::overwrite_existing);

    const std::string containerImage = resolve_live_image(row.image);
    const std::string runId = environment_value("LIVE_RUN_ID").value_or(options.workRoot.filename().string());
    const std::string containerName = "rqp-live-" + runId + "-" + row.id;

    std::vector<std::string> runArgs;
    append_volume_mount(runArgs, bundleDir, "/opt/reqpack", false);
    append_volume_mount(runArgs, workspaceDir, "/workspace", true);
    if (row.mode == "local") {
        append_volume_mount(runArgs, fixturesRoot, "/workspace/live-fixtures", false);
    }
    append_volume_mount(runArgs, scenarioDir, "/workspace/test", false);
    runArgs.push_back(containerImage);
    runArgs.push_back("bash");
    runArgs.push_back("-lc");
    runArgs.push_back(inner);

    live_log(logger, "row " + row.id + ": " + containerImage + " system=" + row.system + " mode=" + row.mode);
    const int status = run_named_container(logger, engine, containerName, runArgs);
    if (status == 0) {
        live_log(logger, "PASS " + row.id);
    } else {
        live_log(logger, "FAIL " + row.id + " (exit " + std::to_string(status) + ")");
    }
    return status;
}

bool parse_live_run_options(const std::vector<std::string>& arguments, LiveRunOptions& options, std::string& error) {
    options.repoRoot = default_repo_root();
    options.matrixFile = options.repoRoot / "tests/system/live/matrix.tsv";
    options.skipBuild = false;
    options.skipImages = false;
    options.prepareOnly = false;
    options.quick = false;
    options.skipPack = false;

    std::size_t index = 0;
    if (index < arguments.size() && arguments[index] == "live") {
        ++index;
    }
    if (index >= arguments.size() || arguments[index] != "run") {
        error = "expected `rqp live run`";
        return false;
    }
    ++index;

    for (; index < arguments.size(); ++index) {
        const std::string& token = arguments[index];
        if (token == "--repo-root") {
            if (index + 1 >= arguments.size()) {
                error = "--repo-root requires a value";
                return false;
            }
            options.repoRoot = std::filesystem::absolute(arguments[++index]);
            options.matrixFile = options.repoRoot / "tests/system/live/matrix.tsv";
            continue;
        }
        if (token == "--bundle") {
            if (index + 1 >= arguments.size()) {
                error = "--bundle requires a value";
                return false;
            }
            options.bundleArchive = std::filesystem::absolute(arguments[++index]);
            options.skipBuild = true;
            continue;
        }
        if (token == "--work-root") {
            if (index + 1 >= arguments.size()) {
                error = "--work-root requires a value";
                return false;
            }
            options.workRoot = std::filesystem::absolute(arguments[++index]);
            continue;
        }
        if (token == "--keep-work") {
            options.keepWorkRoot = true;
            continue;
        }
        if (token == "--prepare-only") {
            options.prepareOnly = true;
            continue;
        }
        if (token == "--skip-build") {
            options.skipBuild = true;
            continue;
        }
        if (token == "--skip-images") {
            options.skipImages = true;
            continue;
        }
        if (token == "--skip-pack") {
            options.skipPack = true;
            continue;
        }
        if (token == "--plugin-rqp") {
            if (index + 1 >= arguments.size()) {
                error = "--plugin-rqp requires a value";
                return false;
            }
            options.pluginPackagePath = std::filesystem::absolute(arguments[++index]);
            continue;
        }
        if (token == "--quick") {
            options.quick = true;
            continue;
        }
        if (token == "--matrix") {
            if (index + 1 >= arguments.size()) {
                error = "--matrix requires a value";
                return false;
            }
            options.matrixFile = std::filesystem::absolute(arguments[++index]);
            continue;
        }
        if (token == "-h" || token == "--help") {
            error = "__help__";
            return false;
        }
        if (!token.empty() && token.rfind("--", 0) == 0) {
            error = "unknown option: " + token;
            return false;
        }
        options.filterIds.push_back(token);
    }

    if (options.workRoot.empty()) {
        std::ostringstream workName;
        workName << "run-" << static_cast<long long>(std::time(nullptr)) << "-" << ::getpid();
        options.workRoot = options.repoRoot / "build/live-system-tests" / workName.str();
        options.ephemeralWorkRoot = true;
    }
    return true;
}

} // namespace

void print_live_run_help(std::ostream& output) {
    output
        << "rqp live - Distro container integration tests\n\n"
        << "Usage:\n"
        << "  rqp live run [options] [matrix-id ...]\n\n"
        << "Options:\n"
        << "  --repo-root PATH     ReqPack checkout (default: REQPACK_REPO_ROOT or build source dir)\n"
        << "  --bundle PATH        Use existing linux release tarball\n"
        << "  --work-root PATH     Reuse prepared work directory (not removed automatically)\n"
        << "  --keep-work          Keep auto-generated --work-root (default: remove when ephemeral)\n"
        << "  --prepare-only       Build bundle and cached distro images, then exit\n"
        << "  --skip-build         Do not compile rqp (requires --bundle or prepared --work-root)\n"
        << "  --skip-images        Do not rebuild cached distro base images\n"
        << "  --skip-pack          Do not run `rqp pack` for plugin-live rows (requires artifact or --plugin-rqp)\n"
        << "  --plugin-rqp PATH    Use this .rqp artifact for plugin-live rows\n"
        << "  --quick              Run only debian-dpkg\n"
        << "  --matrix FILE        Alternate matrix TSV\n"
        << "  -h, --help           Show this help\n\n"
        << "plugin-live rows pack plugins on the host, install .rqp in the container, then run\n"
        << "`rqp test-plugin --live` against the installed plugin id.\n";
}

bool is_live_command(const std::vector<std::string>& arguments) {
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        ReqPackConfigOverrides ignored;
        std::size_t configIndex = i;
        if (consume_cli_config_flag(arguments, configIndex, ignored)) {
            i = configIndex;
            continue;
        }
        return arguments[i] == "live";
    }
    return false;
}

int run_live_command(ReqPackConfig& config,
                     Logger& logger,
                     const std::filesystem::path& /*configPath*/,
                     const std::vector<std::string>& rawArguments) {
    LiveRunOptions options;
    std::string error;
    if (!parse_live_run_options(rawArguments, options, error)) {
        if (error == "__help__") {
            print_live_run_help(std::cout);
            logger.flushSync();
            return 0;
        }
        live_log(logger, error);
        print_live_run_help(std::cerr);
        logger.flushSync();
        return 1;
    }

    const std::optional<ContainerEngine> engine = detect_container_engine();
    if (!engine.has_value()) {
        live_log(logger, "skip: neither podman nor docker available");
        logger.flushSync();
        return 77;
    }
    live_log(logger, "container engine: " + engine->binary);

    if (!std::filesystem::is_directory(plugins_root(options.repoRoot))) {
        live_log(logger, "reqpack-plugins not found near " + options.repoRoot.string());
        live_log(logger, "Set REQPACK_PLUGINS_ROOT to the plugins monorepo checkout.");
        logger.flushSync();
        return 1;
    }

    if (environment_value("REQPACK_LIVE_KEEP_WORK") == "1") {
        options.keepWorkRoot = true;
    }

    const EphemeralWorkRootCleanup ephemeralWorkRootCleanup(logger, options);

    std::filesystem::create_directories(options.workRoot);
    const std::filesystem::path bundleDir = options.workRoot / "bundle";
    const std::filesystem::path fixturesRoot = options.repoRoot / "tests/system/live/fixtures";
    const std::filesystem::path liveRoot = options.repoRoot / "tests/system/live";
    std::filesystem::create_directories(bundleDir);
    std::filesystem::create_directories(options.workRoot / "reports");

    const std::vector<LiveMatrixRow> rows = load_matrix(options.matrixFile);
    if (!environment_value("REQPACK_LIVE_BUNDLE_IMAGE")) {
        for (const LiveMatrixRow& row : rows) {
            if (!row.enabled || !row_selected(options, row.id)) {
                continue;
            }
            if (row.image.find(":nix") != std::string::npos || row.system == "nix") {
                live_log(logger, "selecting fedora bundle build for nix live row(s)");
                setenv("REQPACK_LIVE_BUNDLE_IMAGE", "fedora:44", 1);
                break;
            }
        }
    }

    if (!prepare_bundle(options, logger, bundleDir, engine)) {
        logger.flushSync();
        return 1;
    }
    if (!ensure_live_images(options, logger, *engine)) {
        logger.flushSync();
        return 1;
    }

    if (options.prepareOnly) {
        live_log(logger, "prepare complete: " + options.workRoot.string());
        logger.logStdout(options.workRoot.string());
        logger.flushSync();
        return 0;
    }

    std::vector<std::string> failed;
    for (const LiveMatrixRow& row : rows) {
        if (!row.enabled || !row_selected(options, row.id)) {
            continue;
        }
        int status = 0;
        if (row.mode == "plugin-live") {
            status = run_plugin_live_row(options, logger, config, *engine, row, bundleDir, fixturesRoot);
        } else if (row.mode == "conformance") {
            status = run_conformance_row(options, logger, config, *engine, row, bundleDir, fixturesRoot, liveRoot);
        } else if (row.mode == "repo" || row.mode == "local") {
            status = run_repo_or_local_row(options, logger, *engine, row, bundleDir, fixturesRoot, liveRoot);
        } else {
            live_log(logger, "unknown live test mode: " + row.mode);
            status = 1;
        }
        std::ofstream(options.workRoot / "reports" / (row.id + ".status")) << status << '\n';
        if (status != 0) {
            failed.push_back(row.id);
        }
    }

    if (!failed.empty()) {
        live_log(logger, "live system tests failed:");
        for (const std::string& id : failed) {
            live_log(logger,
                     "  " + id + " (re-run with --keep-work to retain " + options.workRoot.string() + "/rows/" +
                         id + ")");
        }
        logger.flushSync();
        return 1;
    }

    live_log(logger, "all live matrix rows passed (work root: " + options.workRoot.string() + ")");
    logger.flushSync();
    return 0;
}
