#pragma once

#include <curl/curl.h>

#include <string>
#include <vector>

std::string reqpack_ca_bundle_path();
void reqpack_apply_curl_ca_bundle(CURL* curl);
std::vector<std::string> reqpack_sanitized_process_environment();
