#include <catch2/catch.hpp>

#include "output/package_info_format.h"

#include <string>

TEST_CASE("package info format deduplicates status and summary fields", "[unit][output][package_info]") {
    PackageInfo info;
    info.system = "rqp";
    info.name = "wuji";
    info.packageId = "wuji@0.1.3-1+r0";
    info.version = "0.1.3-1+r0";
    info.status = "installed";
    info.installed = "true";
    info.summary = "Unified AI CLI";
    info.description = "Unified AI CLI";
    info.packageType = "package";
    info.architecture = "x86_64";
    info.targetSystems = "linux";
    info.license = "MIT";
    info.author = "Coditary";
    info.email = "Matographo@gmail.com";
    info.publishedAt = "2026-09-16T17:53:17Z";
    info.homepage = "https://github.com/Coditary/wuji-ai/releases/download/v0.1.3/wuji-0.1.3-linux-x86_64.rqp";
    info.sourceUrl = "https://github.com/Coditary/wuji-ai";
    info.repository = "https://example.com/Coditary/wuji-registry/main/wuji-ai-index.json";
    info.tags = {"cli", "ai", "wuji"};

    const std::string rendered = format_package_info(info);

    CHECK(rendered.find("Package ID") == std::string::npos);
    CHECK(rendered.find("Installed:") == std::string::npos);
    CHECK(rendered.find("Status:") == std::string::npos);
    CHECK(rendered.find("Installed") != std::string::npos);
    CHECK(rendered.find("Unified AI CLI") != std::string::npos);
    CHECK(rendered.find("Description") == std::string::npos);
    CHECK(rendered.find("Coditary <Matographo@gmail.com>") != std::string::npos);
    CHECK(rendered.find("Artifact") != std::string::npos);
    CHECK(rendered.find("Source") != std::string::npos);
    CHECK(rendered.find("Registry") != std::string::npos);
    CHECK(rendered.find("#cli") != std::string::npos);
    CHECK(rendered.find("● Installed") != std::string::npos);
    CHECK(rendered.find("Tags") != std::string::npos);
    CHECK(rendered.find("Metadata") != std::string::npos);
    CHECK(rendered.find("Links") != std::string::npos);
}

TEST_CASE("package info format shows update badge when latest differs", "[unit][output][package_info]") {
    PackageInfo info;
    info.name = "demo";
    info.version = "1.0.0";
    info.latestVersion = "2.0.0";
    info.installed = "true";
    info.summary = "Demo package";

    const std::string rendered = format_package_info(info);
    CHECK(rendered.find("Update available") != std::string::npos);
}
