#include <catch2/catch.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include "core/rq_package.h"
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
        std::string hash = overrideHash.value_or({});
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
