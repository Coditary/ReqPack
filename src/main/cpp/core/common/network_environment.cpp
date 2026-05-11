#include "core/common/network_environment.h"

#include <cstdlib>
#include <filesystem>
#include <string_view>

extern char** environ;

namespace {

bool starts_with(const std::string& value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::string env_value_if_non_empty(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return {};
    }
    return value;
}

}  // namespace

std::string reqpack_ca_bundle_path() {
    for (const char* envName : {"SSL_CERT_FILE", "CURL_CA_BUNDLE", "GIT_SSL_CAINFO"}) {
        const std::string configured = env_value_if_non_empty(envName);
        if (!configured.empty() && file_exists(configured)) {
            return configured;
        }
    }

    for (const char* candidate : {
             "/etc/ssl/cert.pem",
             "/etc/ssl/certs/ca-certificates.crt",
             "/etc/ssl/certs/ca-bundle.crt",
             "/etc/pki/tls/certs/ca-bundle.crt",
             "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
         }) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

void reqpack_apply_curl_ca_bundle(CURL* curl) {
    if (curl == nullptr) {
        return;
    }

    const std::string caBundlePath = reqpack_ca_bundle_path();
    if (!caBundlePath.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath.c_str());
    }
}

std::vector<std::string> reqpack_sanitized_process_environment() {
    std::vector<std::string> environment;
    bool hasSslCertFile = false;
    bool hasCurlCaBundle = false;
    bool hasGitSslCaInfo = false;
    environment.reserve(64);

    for (char** current = environ; current != nullptr && *current != nullptr; ++current) {
        const std::string entry(*current);
        if (starts_with(entry, "LD_LIBRARY_PATH=") || starts_with(entry, "DYLD_LIBRARY_PATH=")) {
            continue;
        }
        if (starts_with(entry, "SSL_CERT_FILE=")) {
            const std::string value = entry.substr(std::string_view{"SSL_CERT_FILE="}.size());
            if (file_exists(value)) {
                hasSslCertFile = true;
                environment.push_back(entry);
            }
            continue;
        } else if (starts_with(entry, "CURL_CA_BUNDLE=")) {
            const std::string value = entry.substr(std::string_view{"CURL_CA_BUNDLE="}.size());
            if (file_exists(value)) {
                hasCurlCaBundle = true;
                environment.push_back(entry);
            }
            continue;
        } else if (starts_with(entry, "GIT_SSL_CAINFO=")) {
            const std::string value = entry.substr(std::string_view{"GIT_SSL_CAINFO="}.size());
            if (file_exists(value)) {
                hasGitSslCaInfo = true;
                environment.push_back(entry);
            }
            continue;
        }
        environment.push_back(entry);
    }

    const std::string caBundlePath = reqpack_ca_bundle_path();
    if (!caBundlePath.empty()) {
        if (!hasSslCertFile) {
            environment.push_back("SSL_CERT_FILE=" + caBundlePath);
        }
        if (!hasCurlCaBundle) {
            environment.push_back("CURL_CA_BUNDLE=" + caBundlePath);
        }
        if (!hasGitSslCaInfo) {
            environment.push_back("GIT_SSL_CAINFO=" + caBundlePath);
        }
    }

    return environment;
}
