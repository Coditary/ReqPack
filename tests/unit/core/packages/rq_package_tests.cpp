#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include "core/packages/rq_package.h"
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

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& target) : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(target);
    }

    ~ScopedCurrentPath() {
        std::error_code error;
        std::filesystem::current_path(original_, error);
    }

private:
    std::filesystem::path original_;
};

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.is_open());
    output << content;
}

std::filesystem::path build_rqp_package(
    const std::filesystem::path& root,
    const std::string& name,
    const std::string& installLua,
    const std::optional<std::pair<std::string, std::string>>& payloadFile,
    const std::optional<std::string>& overrideHash = std::nullopt
) {
    const std::filesystem::path packageRoot = root / (name + "-pkg");
    const std::filesystem::path payloadRoot = packageRoot / "payload-tree";
    const std::filesystem::path controlRoot = packageRoot / "control";
    std::filesystem::create_directories(controlRoot / "scripts");
    std::filesystem::create_directories(controlRoot / "hashes");
    std::filesystem::create_directories(controlRoot / "payload");

    if (payloadFile.has_value()) {
        write_file(payloadRoot / payloadFile->first, payloadFile->second);
        const std::string payloadTar = (controlRoot / "payload" / "payload.tar").string();
        const std::string payloadTarZst = (controlRoot / "payload" / "payload.tar.zst").string();
        REQUIRE(std::system(("tar -C " + escape_shell_arg(payloadRoot.string()) + " -cf " + escape_shell_arg(payloadTar) + " .").c_str()) == 0);
        REQUIRE(std::system(("zstd -q -f " + escape_shell_arg(payloadTar) + " -o " + escape_shell_arg(payloadTarZst)).c_str()) == 0);
        std::string hash = overrideHash.value_or("");
        if (!overrideHash.has_value()) {
            const std::string hashOutput = run_command_capture("openssl dgst -sha256 " + escape_shell_arg(payloadTarZst));
            const std::size_t pos = hashOutput.rfind(' ');
            REQUIRE(pos != std::string::npos);
            hash = hashOutput.substr(pos + 1, 64);
        }
        write_file(controlRoot / "hashes" / "payload.sha256", hash + "  payload/payload.tar.zst\n");
    }

    const std::string metadata = payloadFile.has_value()
        ? "{\n"
          "  \"formatVersion\": 1,\n"
          "  \"name\": \"" + name + "\",\n"
          "  \"version\": \"1.0.0\",\n"
          "  \"release\": 1,\n"
          "  \"revision\": 0,\n"
          "  \"summary\": \"test package\",\n"
          "  \"description\": \"reader test package\",\n"
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
          "  \"version\": \"1.0.0\",\n"
          "  \"release\": 1,\n"
          "  \"revision\": 0,\n"
          "  \"summary\": \"test package\",\n"
          "  \"description\": \"reader test package\",\n"
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
    REQUIRE(std::system(("tar -C " + escape_shell_arg(controlRoot.string()) + " -cf " + escape_shell_arg(packagePath.string()) + " .").c_str()) == 0);
    return packagePath;
}

std::string base_metadata_json(const std::string& name, const std::optional<std::string>& payloadBlock = std::nullopt) {
    std::string metadata =
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"" + name + "\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"release\": 1,\n"
        "  \"revision\": 0,\n"
        "  \"summary\": \"test package\",\n"
        "  \"description\": \"reader test package\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"vendor\": \"ReqPack Tests\",\n"
        "  \"maintainerEmail\": \"tests@example.org\",\n"
        "  \"url\": \"https://example.test/" + name + ".rqp\"";
    if (payloadBlock.has_value()) {
        metadata += ",\n  \"payload\": " + payloadBlock.value();
    }
    metadata += "\n}\n";
    return metadata;
}

void write_pack_project(const std::filesystem::path& root, const std::string& name) {
    write_file(root / "metadata.json", base_metadata_json(name));
    write_file(root / "reqpack.lua", R"(
return {
  apiVersion = 1,
  hooks = {
    install = "scripts/install.lua"
  }
}
)");
    write_file(root / "scripts" / "install.lua", "return true\n");
}

std::string payload_block_json() {
    return
        "{\n"
        "    \"path\": \"payload/payload.tar.zst\",\n"
        "    \"archive\": \"tar\",\n"
        "    \"compression\": \"zstd\",\n"
        "    \"hashAlgorithm\": \"sha256\",\n"
        "    \"hashFile\": \"hashes/payload.sha256\",\n"
        "    \"sizeCompressed\": 0,\n"
        "    \"sizeInstalledExpected\": 0\n"
        "  }";
}

}  // namespace

TEST_CASE("rqp package reader loads valid rqp and extracts payload", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-reader-valid"};
    const std::filesystem::path packagePath = build_rqp_package(
        tempDir.path(),
        "valid",
        "return true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello"))
    );

    const RqPackageLayout layout = RqPackageReader::load(packagePath, tempDir.path() / "work", tempDir.path() / "state");

    CHECK(layout.metadata.name == "valid");
    CHECK(layout.identity == "valid@1.0.0-1+r0");
    CHECK(layout.hasPayload);
    CHECK(std::filesystem::exists(layout.payloadDir / "payload.txt"));
}

TEST_CASE("rqp package reader rejects payload hash mismatch", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-reader-bad-hash"};
    const std::filesystem::path packagePath = build_rqp_package(
        tempDir.path(),
        "bad-hash",
        "return true\n",
        std::make_pair(std::string("payload.txt"), std::string("hello")),
        std::string(64, 'a')
    );

    REQUIRE_THROWS_WITH(
        RqPackageReader::load(packagePath, tempDir.path() / "work", tempDir.path() / "state"),
        Catch::Matchers::Contains("payload sha256 mismatch")
    );
}

TEST_CASE("rqp metadata parser normalizes missing architecture and system", "[unit][rq_package][core]") {
    const RqMetadata metadata = rq_parse_metadata_json(
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"portable\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"release\": 1,\n"
        "  \"revision\": 0,\n"
        "  \"summary\": \"portable\",\n"
        "  \"description\": \"portable\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"vendor\": \"ReqPack Tests\",\n"
        "  \"maintainerEmail\": \"tests@example.org\",\n"
        "  \"url\": \"https://example.test/portable.rqp\"\n"
        "}\n"
    );

    CHECK(metadata.architecture == "noarch");
    CHECK(metadata.systems == std::vector<std::string>{"nosys"});
}

TEST_CASE("rqp metadata parser accepts system string and array", "[unit][rq_package][core]") {
    const RqMetadata stringMetadata = rq_parse_metadata_json(
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"debian-tool\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"release\": 1,\n"
        "  \"revision\": 0,\n"
        "  \"summary\": \"tool\",\n"
        "  \"description\": \"tool\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"architecture\": \"\",\n"
        "  \"system\": \"Debian\",\n"
        "  \"vendor\": \"ReqPack Tests\",\n"
        "  \"maintainerEmail\": \"tests@example.org\",\n"
        "  \"url\": \"https://example.test/debian-tool.rqp\"\n"
        "}\n"
    );

    const RqMetadata arrayMetadata = rq_parse_metadata_json(
        "{\n"
        "  \"formatVersion\": 1,\n"
        "  \"name\": \"multi-tool\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"release\": 1,\n"
        "  \"revision\": 0,\n"
        "  \"summary\": \"tool\",\n"
        "  \"description\": \"tool\",\n"
        "  \"license\": \"MIT\",\n"
        "  \"architecture\": \"noarch\",\n"
        "  \"system\": [\"ubuntu\", \"linux\", \"Ubuntu\"],\n"
        "  \"vendor\": \"ReqPack Tests\",\n"
        "  \"maintainerEmail\": \"tests@example.org\",\n"
        "  \"url\": \"https://example.test/multi-tool.rqp\"\n"
        "}\n"
    );

    CHECK(stringMetadata.systems == std::vector<std::string>{"debian"});
    CHECK(stringMetadata.architecture == "noarch");
    CHECK(arrayMetadata.systems == std::vector<std::string>{"linux", "ubuntu"});
}

TEST_CASE("rqp system matching supports aliases and nosys", "[unit][rq_package][core]") {
    const auto aliases = rq_builtin_system_aliases();

    CHECK(rq_system_matches({"nosys"}, std::set<std::string>{"fedora", "linux"}, aliases));
    CHECK(rq_system_matches({"debian-family"}, std::set<std::string>{"ubuntu", "linux"}, aliases));
    CHECK(rq_system_matches({"darwin"}, std::set<std::string>{"macos", "darwin"}, aliases));
    CHECK_FALSE(rq_system_matches({"debian"}, std::set<std::string>{"fedora", "linux"}, aliases));
}

TEST_CASE("rqp package builder builds control only package", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-control"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "control-only");

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = projectRoot,
        .outputPath = tempDir.path() / "dist" / "control-only.rqp",
        .force = true,
        .interactive = false,
    });

    CHECK(result.metadata.name == "control-only");
    CHECK_FALSE(result.hasPayload);
    CHECK(std::filesystem::exists(result.outputPath));

    const RqPackageLayout layout = RqPackageReader::load(result.outputPath, tempDir.path() / "work", tempDir.path() / "state", default_reqpack_config(), false);
    CHECK(layout.metadata.name == "control-only");
    CHECK_FALSE(layout.hasPayload);
}

TEST_CASE("rqp package builder defaults output into current project directory", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-default-output"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "default-output");
    const ScopedCurrentPath scopedCurrentPath{projectRoot};

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = ".",
        .force = true,
        .interactive = false,
    });

    CHECK(result.outputPath.lexically_normal() == (projectRoot / "default-output.rqp").lexically_normal());
    CHECK(std::filesystem::exists(result.outputPath));
}

TEST_CASE("rqp package builder keeps default output next to explicit project path caller", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-default-output-explicit"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "explicit-output");
    const ScopedCurrentPath scopedCurrentPath{tempDir.path()};

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = "./project",
        .force = true,
        .interactive = false,
    });

    CHECK(result.outputPath.lexically_normal() == (tempDir.path() / "explicit-output.rqp").lexically_normal());
    CHECK(std::filesystem::exists(result.outputPath));
}

TEST_CASE("rqp package builder builds payload from payload tree", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-payload-tree"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "payload-tree-demo");
    write_file(projectRoot / "payload-tree" / "bin" / "demo.txt", "hello world");

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = projectRoot,
        .outputPath = tempDir.path() / "payload-tree-demo.rqp",
        .force = true,
        .interactive = false,
    });

    REQUIRE(result.metadata.payload.has_value());
    CHECK(result.hasPayload);
    CHECK(result.metadata.payload->path == "payload/payload.tar.zst");
    CHECK(result.metadata.payload->hashFile == "hashes/payload.sha256");

    const RqPackageLayout layout = RqPackageReader::load(result.outputPath, tempDir.path() / "work", tempDir.path() / "state", default_reqpack_config(), false);
    CHECK(layout.hasPayload);
    CHECK(std::filesystem::exists(layout.payloadDir / "bin" / "demo.txt"));
}

TEST_CASE("rqp package builder accepts external payload dir", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-external-payload"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    const std::filesystem::path payloadRoot = tempDir.path() / "rootfs";
    write_pack_project(projectRoot, "external-payload-demo");
    write_file(payloadRoot / "etc" / "demo.conf", "x=1\n");

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = projectRoot,
        .outputPath = tempDir.path() / "external-payload-demo.rqp",
        .payloadRoot = payloadRoot,
        .force = true,
        .interactive = false,
    });

    CHECK(result.hasPayload);
    const RqPackageLayout layout = RqPackageReader::load(result.outputPath, tempDir.path() / "work", tempDir.path() / "state", default_reqpack_config(), false);
    CHECK(std::filesystem::exists(layout.payloadDir / "etc" / "demo.conf"));
}

TEST_CASE("rqp package builder rebuilds validated prebuilt payload", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-prebuilt"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "prebuilt-demo");
    write_file(projectRoot / "metadata.json", base_metadata_json("prebuilt-demo", payload_block_json()));
    write_file(projectRoot / "payload-tree" / "demo.txt", "hello");
    std::filesystem::create_directories(projectRoot / "payload");

    const std::string payloadTar = (tempDir.path() / "payload.tar").string();
    const std::string payloadTarZst = (projectRoot / "payload" / "payload.tar.zst").string();
    REQUIRE(std::system(("tar -C " + escape_shell_arg((projectRoot / "payload-tree").string()) + " -cf " + escape_shell_arg(payloadTar) + " .").c_str()) == 0);
    REQUIRE(std::system(("zstd -q -f " + escape_shell_arg(payloadTar) + " -o " + escape_shell_arg(payloadTarZst)).c_str()) == 0);
    const std::string hashOutput = run_command_capture("openssl dgst -sha256 " + escape_shell_arg(payloadTarZst));
    const std::size_t pos = hashOutput.rfind(' ');
    REQUIRE(pos != std::string::npos);
    const std::string hash = hashOutput.substr(pos + 1, 64);
    write_file(projectRoot / "hashes" / "payload.sha256", hash + "  payload/payload.tar.zst\n");
    std::filesystem::remove_all(projectRoot / "payload-tree");

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = projectRoot,
        .outputPath = tempDir.path() / "prebuilt-demo.rqp",
        .force = true,
        .interactive = false,
    });

    CHECK(result.hasPayload);
    const RqPackageLayout layout = RqPackageReader::load(result.outputPath, tempDir.path() / "work", tempDir.path() / "state", default_reqpack_config(), false);
    CHECK(std::filesystem::exists(layout.payloadDir / "demo.txt"));
}

TEST_CASE("rqp package builder rejects ambiguous payload sources", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-conflict"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "ambiguous-demo");
    write_file(projectRoot / "payload-tree" / "demo.txt", "hello");
    std::filesystem::create_directories(projectRoot / "payload");
    std::filesystem::create_directories(projectRoot / "hashes");

    REQUIRE_THROWS_WITH(
        rq_build_package({
            .projectRoot = projectRoot,
            .outputPath = tempDir.path() / "ambiguous-demo.rqp",
            .force = true,
            .interactive = false,
        }),
        Catch::Matchers::Contains("payload-tree/")
    );
}

TEST_CASE("rqp package builder skips host compatibility during self validation", "[unit][rq_package][core]") {
    TempDir tempDir{"reqpack-rqp-pack-cross-target"};
    const std::filesystem::path projectRoot = tempDir.path() / "project";
    write_pack_project(projectRoot, "cross-target");
    write_file(projectRoot / "metadata.json",
               "{\n"
               "  \"formatVersion\": 1,\n"
               "  \"name\": \"cross-target\",\n"
               "  \"version\": \"1.0.0\",\n"
               "  \"release\": 1,\n"
               "  \"revision\": 0,\n"
               "  \"summary\": \"test\",\n"
               "  \"description\": \"test\",\n"
               "  \"license\": \"MIT\",\n"
               "  \"architecture\": \"mips64\",\n"
               "  \"system\": [\"imaginary-os\"],\n"
               "  \"vendor\": \"ReqPack Tests\",\n"
               "  \"maintainerEmail\": \"tests@example.org\",\n"
               "  \"url\": \"https://example.test/cross-target.rqp\"\n"
               "}\n");

    const RqPackageBuildResult result = rq_build_package({
        .projectRoot = projectRoot,
        .outputPath = tempDir.path() / "cross-target.rqp",
        .force = true,
        .interactive = false,
    });
    CHECK(std::filesystem::exists(result.outputPath));

    REQUIRE_THROWS_WITH(
        RqPackageReader::load(result.outputPath, tempDir.path() / "work-host", tempDir.path() / "state-host"),
        Catch::Matchers::Contains("package architecture does not match host")
    );
}
