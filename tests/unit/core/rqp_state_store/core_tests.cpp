#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/rqp_state_store.h"

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

void write_installed_state(const std::filesystem::path& root, const std::string& name, const std::string& version, int release, int revision) {
    const std::filesystem::path stateDir = root / name / (name + "@" + version + "-" + std::to_string(release) + "+r" + std::to_string(revision));
    write_file(stateDir / "metadata.json",
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"" + name + "\",\n"
        "  \"version\": \"" + version + "\",\n"
        "  \"release\": " + std::to_string(release) + ",\n"
        "  \"revision\": " + std::to_string(revision) + ",\n"
        "  \"summary\": \"summary\",\n"
        "  \"description\": \"description\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"architecture\": \"noarch\",\n"
        "  \"vendor\": \"ReqPack Tests\",\n"
        "  \"maintainerEmail\": \"tests@example.org\",\n"
        "  \"tags\": [\"test\"],\n"
        "  \"url\": \"https://example.test/" + name + ".rqp\"\n"
        "}\n");
    write_file(stateDir / "reqpack.lua", R"(
return {
  apiVersion = 1,
  hooks = {
    install = "scripts/install.lua"
  }
}
)" );
    write_file(stateDir / "scripts" / "install.lua", "return true\n");
    write_file(stateDir / "source.json",
        "{\n"
        "  \"source\": \"repository\",\n"
        "  \"path\": \"" + name + "@" + version + "\",\n"
        "  \"repository\": \"file:///tmp/index.json\",\n"
        "  \"identity\": \"" + name + "@" + version + "-" + std::to_string(release) + "+r" + std::to_string(revision) + "\"\n"
        "}\n");
    write_file(stateDir / "manifest.json", "[]\n");
}

}  // namespace

TEST_CASE("rqp state store lists installed packages in stable order", "[unit][rqp_state_store][core]") {
    TempDir tempDir{"reqpack-rqp-state-list"};
    ReqPackConfig config = default_reqpack_config();
    config.rqp.statePath = tempDir.path().string();
    write_installed_state(tempDir.path(), "zeta", "1.0.0", 1, 0);
    write_installed_state(tempDir.path(), "alpha", "2.0.0", 1, 0);

    const std::vector<RqpInstalledPackage> installed = RqpStateStore(config).listInstalled();

    REQUIRE(installed.size() == 2);
    CHECK(installed[0].metadata.name == "alpha");
    CHECK(installed[1].metadata.name == "zeta");
}

TEST_CASE("rqp state store finds exact version match", "[unit][rqp_state_store][core]") {
    TempDir tempDir{"reqpack-rqp-state-find"};
    ReqPackConfig config = default_reqpack_config();
    config.rqp.statePath = tempDir.path().string();
    write_installed_state(tempDir.path(), "tool", "1.0.0", 1, 0);
    write_installed_state(tempDir.path(), "tool", "2.0.0", 1, 0);

    const std::vector<RqpInstalledPackage> installed = RqpStateStore(config).findInstalled("tool", "2.0.0");

    REQUIRE(installed.size() == 1);
    CHECK(installed.front().metadata.version == "2.0.0");
    CHECK(installed.front().source.source == "repository");
}

TEST_CASE("rqp state store removes installed state and prunes empty package directory", "[unit][rqp_state_store][core]") {
    TempDir tempDir{"reqpack-rqp-state-remove"};
    ReqPackConfig config = default_reqpack_config();
    config.rqp.statePath = tempDir.path().string();
    write_installed_state(tempDir.path(), "tool", "1.0.0", 1, 0);

    RqpStateStore store(config);
    const std::vector<RqpInstalledPackage> installed = store.findInstalled("tool", "1.0.0");
    REQUIRE(installed.size() == 1);

    REQUIRE(store.removeInstalledState(installed.front()));
    CHECK_FALSE(std::filesystem::exists(tempDir.path() / "tool"));
}
