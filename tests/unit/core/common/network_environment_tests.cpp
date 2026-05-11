#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "core/common/network_environment.h"

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

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(std::string name)
        : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str())) {
            previous_ = std::string(existing);
        }
    }

    ScopedEnvVar(std::string name, std::string value)
        : ScopedEnvVar(std::move(name)) {
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

bool contains_prefix(const std::vector<std::string>& values, const std::string& prefix) {
    return std::any_of(values.begin(), values.end(), [&prefix](const std::string& value) {
        return value.rfind(prefix, 0) == 0;
    });
}

}  // namespace

TEST_CASE("network environment prefers configured CA bundle when valid", "[unit][network][env]") {
    TempDir tempDir{"reqpack-network-env"};
    const std::filesystem::path bundlePath = tempDir.path() / "ca-bundle.pem";
    std::ofstream output(bundlePath);
    REQUIRE(output.is_open());
    output << "test";
    output.close();

    ScopedEnvVar sslCertFile{"SSL_CERT_FILE", bundlePath.string()};
    ScopedEnvVar curlCaBundle{"CURL_CA_BUNDLE"};
    ScopedEnvVar gitSslCaInfo{"GIT_SSL_CAINFO"};

    CHECK(reqpack_ca_bundle_path() == bundlePath.string());
}

TEST_CASE("network environment strips bundled library paths and seeds CA vars", "[unit][network][env]") {
    TempDir tempDir{"reqpack-network-sanitize"};
    const std::filesystem::path bundlePath = tempDir.path() / "ca-bundle.pem";
    std::ofstream output(bundlePath);
    REQUIRE(output.is_open());
    output << "test";
    output.close();

    ScopedEnvVar ldLibraryPath{"LD_LIBRARY_PATH", "/tmp/reqpack/lib"};
    ScopedEnvVar dyldLibraryPath{"DYLD_LIBRARY_PATH", "/tmp/reqpack/lib"};
    ScopedEnvVar sslCertFile{"SSL_CERT_FILE", bundlePath.string()};
    ScopedEnvVar curlCaBundle{"CURL_CA_BUNDLE"};
    ScopedEnvVar gitSslCaInfo{"GIT_SSL_CAINFO"};

    const std::vector<std::string> environment = reqpack_sanitized_process_environment();
    CHECK_FALSE(contains_prefix(environment, "LD_LIBRARY_PATH="));
    CHECK_FALSE(contains_prefix(environment, "DYLD_LIBRARY_PATH="));
    CHECK(contains_prefix(environment, "SSL_CERT_FILE="));
    CHECK(contains_prefix(environment, "CURL_CA_BUNDLE="));
    CHECK(contains_prefix(environment, "GIT_SSL_CAINFO="));
}
