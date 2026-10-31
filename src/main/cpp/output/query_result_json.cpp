#include "output/query_result_json.h"

#include "output/logger.h"
#include "output/run_result_json.h"

#include <rqp/security/json_util.h>

#include <iostream>
#include <sstream>
#include <utility>

namespace {

void append_json_string_field(std::ostringstream& stream, const std::string& key, const std::string& value,
                              bool& firstField) {
	if (value.empty()) {
		return;
	}
	if (!firstField) {
		stream << ',';
	}
	firstField = false;
	stream << '"' << rqp_json_escape(key) << "\":\"" << rqp_json_escape(value) << '"';
}

void append_json_string_list_field(std::ostringstream& stream, const std::string& key,
                                   const std::vector<std::string>& values, bool& firstField) {
	if (values.empty()) {
		return;
	}
	if (!firstField) {
		stream << ',';
	}
	firstField = false;
	stream << '"' << rqp_json_escape(key) << "\":[";
	for (std::size_t index = 0; index < values.size(); ++index) {
		if (index > 0) {
			stream << ',';
		}
		stream << '"' << rqp_json_escape(values[index]) << '"';
	}
	stream << ']';
}

std::string query_status_for_action(ActionType action) {
	switch (action) {
		case ActionType::SEARCH:
			return "found";
		case ActionType::OUTDATED:
			return "outdated";
		case ActionType::LIST:
		default:
			return "listed";
	}
}

std::string package_version_for_query_item(ActionType action, const PackageInfo& item) {
	if (action == ActionType::OUTDATED) {
		if (!item.installed.empty()) {
			return item.installed;
		}
	}
	if (!item.version.empty()) {
		return item.version;
	}
	return item.installed;
}

std::string serialize_package_info_object(const PackageInfo& info) {
	std::ostringstream stream;
	stream << '{';
	bool firstField = true;
	append_json_string_field(stream, "system", info.system, firstField);
	append_json_string_field(stream, "name", info.name, firstField);
	append_json_string_field(stream, "packageId", info.packageId, firstField);
	append_json_string_field(stream, "version", info.version, firstField);
	append_json_string_field(stream, "latestVersion", info.latestVersion, firstField);
	append_json_string_field(stream, "status", info.status, firstField);
	append_json_string_field(stream, "installed", info.installed, firstField);
	append_json_string_field(stream, "summary", info.summary, firstField);
	append_json_string_field(stream, "description", info.description, firstField);
	append_json_string_field(stream, "homepage", info.homepage, firstField);
	append_json_string_field(stream, "documentation", info.documentation, firstField);
	append_json_string_field(stream, "sourceUrl", info.sourceUrl, firstField);
	append_json_string_field(stream, "repository", info.repository, firstField);
	append_json_string_field(stream, "channel", info.channel, firstField);
	append_json_string_field(stream, "section", info.section, firstField);
	append_json_string_field(stream, "packageType", info.packageType, firstField);
	append_json_string_field(stream, "architecture", info.architecture, firstField);
	append_json_string_field(stream, "targetSystems", info.targetSystems, firstField);
	append_json_string_field(stream, "license", info.license, firstField);
	append_json_string_field(stream, "author", info.author, firstField);
	append_json_string_field(stream, "maintainer", info.maintainer, firstField);
	append_json_string_field(stream, "email", info.email, firstField);
	append_json_string_field(stream, "publishedAt", info.publishedAt, firstField);
	append_json_string_field(stream, "updatedAt", info.updatedAt, firstField);
	append_json_string_field(stream, "size", info.size, firstField);
	append_json_string_field(stream, "installedSize", info.installedSize, firstField);
	append_json_string_list_field(stream, "dependencies", info.dependencies, firstField);
	append_json_string_list_field(stream, "optionalDependencies", info.optionalDependencies, firstField);
	append_json_string_list_field(stream, "provides", info.provides, firstField);
	append_json_string_list_field(stream, "conflicts", info.conflicts, firstField);
	append_json_string_list_field(stream, "replaces", info.replaces, firstField);
	append_json_string_list_field(stream, "binaries", info.binaries, firstField);
	append_json_string_list_field(stream, "tags", info.tags, firstField);
	for (const auto& [key, value] : info.extraFields) {
		append_json_string_field(stream, key, value, firstField);
	}
	stream << '}';
	return stream.str();
}

}  // namespace

RunResultJsonDocument build_query_packages_document(ActionType action, const std::vector<PackageInfo>& items) {
	RunResultJsonDocument document;
	document.ok = true;
	document.dryRun = false;
	document.items.reserve(items.size());
	const std::string status = query_status_for_action(action);
	for (const PackageInfo& item : items) {
		RunResultJsonItem jsonItem;
		jsonItem.system = item.system;
		jsonItem.name = item.name;
		jsonItem.version = package_version_for_query_item(action, item);
		if (action == ActionType::OUTDATED && !item.latestVersion.empty()) {
			jsonItem.latestVersion = item.latestVersion;
		}
		jsonItem.status = status;
		document.items.push_back(std::move(jsonItem));
	}
	return document;
}

void emit_query_packages_json(ActionType action, const std::vector<PackageInfo>& items) {
	emit_run_result_json(build_query_packages_document(action, items));
}

void emit_query_info_json(const Request& request, const PackageInfo& item, const bool success) {
	PackageInfo normalized = item;
	if (normalized.system.empty()) {
		normalized.system = request.system;
	}
	if (normalized.name.empty() && !request.packages.empty()) {
		normalized.name = request.packages.front();
	}

	std::ostringstream stream;
	stream << '{';
	stream << "\"ok\":" << (success ? "true" : "false");
	stream << ",\"dryRun\":false";
	if (!success) {
		stream << ",\"errorMessage\":\"No package info found\"";
	}
	stream << ",\"info\":" << serialize_package_info_object(normalized);
	stream << '}';

	Logger::instance().flushSync();
	std::cout << stream.str() << '\n';
	std::cout.flush();
}

void emit_local_rqp_info_json(const ReqPackConfig& config, const std::string& version) {
	PackageInfo info;
	info.system = "rqp";
	info.name = config.applicationName;
	info.version = version;
	info.description = "ReqPack self metadata";

	std::ostringstream stream;
	stream << '{';
	stream << "\"ok\":true";
	stream << ",\"dryRun\":false";
	stream << ",\"info\":" << serialize_package_info_object(info);
	stream << '}';

	Logger::instance().flushSync();
	std::cout << stream.str() << '\n';
	std::cout.flush();
}
