#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include "core/history_manager.h"
#include "core/snapshot_exporter.h"

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

ReqPackConfig make_history_config(const std::filesystem::path& historyPath) {
    ReqPackConfig config;
    config.history.enabled = false;
    config.history.trackInstalled = true;
    config.history.historyPath = historyPath.string();
    return config;
}

std::optional<InstalledEntry> find_entry(
    const std::vector<InstalledEntry>& entries,
    const std::string& system,
    const std::string& name
) {
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const InstalledEntry& entry) {
        return entry.system == system && entry.name == name;
    });
    if (it == entries.end()) {
        return std::nullopt;
    }
    return *it;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("history manager stores installed state in LMDB backend", "[unit][history][installed_state]") {
    TempDir tempDir{"reqpack-history-db"};
    const ReqPackConfig config = make_history_config(tempDir.path());
    HistoryManager history(config);

    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T12:00:00Z",
        .action = "install",
        .packageName = "ripgrep",
        .packageVersion = "14.1",
        .system = "dnf",
        .status = "success"
    }));
    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T12:01:00Z",
        .action = "install",
        .packageName = "eslint",
        .packageVersion = "9.0.0",
        .system = "npm",
        .status = "success"
    }));
    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T12:02:00Z",
        .action = "update",
        .packageName = "ripgrep",
        .packageVersion = "14.2",
        .system = "dnf",
        .status = "success"
    }));
    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T12:03:00Z",
        .action = "install",
        .packageName = "ripgrep",
        .packageVersion = "99.0",
        .system = "dnf",
        .status = "failed"
    }));

    std::vector<InstalledEntry> entries = history.loadInstalledState();
    REQUIRE(entries.size() == 2);
    const std::optional<InstalledEntry> ripgrep = find_entry(entries, "dnf", "ripgrep");
    REQUIRE(ripgrep.has_value());
    CHECK(ripgrep->version == "14.2");
    const std::optional<InstalledEntry> eslint = find_entry(entries, "npm", "eslint");
    REQUIRE(eslint.has_value());
    CHECK(eslint->version == "9.0.0");
    CHECK(std::filesystem::exists(tempDir.path()));
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "installed.json"));

    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T12:04:00Z",
        .action = "remove",
        .packageName = "eslint",
        .system = "npm",
        .status = "success"
    }));

    entries = history.loadInstalledState();
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(find_entry(entries, "npm", "eslint").has_value());
}

TEST_CASE("history manager imports legacy installed json only once", "[unit][history][migration]") {
    TempDir tempDir{"reqpack-history-legacy"};
    const ReqPackConfig config = make_history_config(tempDir.path());

    {
        std::ofstream legacy(tempDir.path() / "installed.json", std::ios::trunc);
        REQUIRE(legacy.is_open());
        legacy << "[\n";
        legacy << "  {\"name\":\"bat\",\"version\":\"0.24.0\",\"manager\":\"dnf\",\"installedAt\":\"2026-04-29T10:00:00Z\"}\n";
        legacy << "]\n";
    }

    HistoryManager history(config);
    std::vector<InstalledEntry> entries = history.loadInstalledState();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().name == "bat");
    CHECK(entries.front().system == "dnf");

    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T10:05:00Z",
        .action = "remove",
        .packageName = "bat",
        .system = "dnf",
        .status = "success"
    }));

    HistoryManager reloaded(config);
    entries = reloaded.loadInstalledState();
    CHECK(entries.empty());
    CHECK(std::filesystem::exists(tempDir.path() / "installed.json"));
}

TEST_CASE("snapshot exporter reads installed state from history database", "[unit][snapshot][history]") {
    TempDir tempDir{"reqpack-snapshot-history"};
    const ReqPackConfig config = make_history_config(tempDir.path());
    HistoryManager history(config);

    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T13:00:00Z",
        .action = "install",
        .packageName = "eslint",
        .packageVersion = "9.0.0",
        .system = "npm",
        .status = "success"
    }));
    REQUIRE(history.updateInstalledState(HistoryEntry{
        .timestamp = "2026-04-29T13:01:00Z",
        .action = "install",
        .packageName = "ripgrep",
        .packageVersion = "14.2",
        .system = "dnf",
        .status = "success"
    }));

    SnapshotExporter exporter(config);
    Request request;
    request.action = ActionType::SNAPSHOT;
    request.outputPath = (tempDir.path() / "reqpack.lua").string();

    REQUIRE(exporter.exportSnapshot(request));

    const std::string rendered = read_file(request.outputPath);
    CHECK(rendered.find("{ system = \"dnf\", name = \"ripgrep\", version = \"14.2\" }") != std::string::npos);
    CHECK(rendered.find("{ system = \"npm\", name = \"eslint\", version = \"9.0.0\" }") != std::string::npos);
    CHECK(rendered.find("system = \"dnf\"") < rendered.find("system = \"npm\""));
}
