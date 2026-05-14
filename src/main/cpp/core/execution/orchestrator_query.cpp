#include "orchestrator_internal.h"

#include "output/command_output.h"

#include <utility>
#include <vector>

namespace {

bool package_info_has_details(const PackageInfo& item) {
	return !item.packageId.empty() || !item.version.empty() || !item.latestVersion.empty() ||
		!item.status.empty() || !item.installed.empty() || !item.summary.empty() || !item.description.empty() ||
		!item.homepage.empty() || !item.documentation.empty() || !item.sourceUrl.empty() || !item.repository.empty() ||
		!item.channel.empty() || !item.section.empty() || !item.packageType.empty() || !item.architecture.empty() || !item.targetSystems.empty() || !item.license.empty() ||
		!item.author.empty() || !item.maintainer.empty() || !item.email.empty() || !item.publishedAt.empty() ||
		!item.updatedAt.empty() || !item.size.empty() || !item.installedSize.empty() ||
		!item.dependencies.empty() || !item.optionalDependencies.empty() || !item.provides.empty() ||
		!item.conflicts.empty() || !item.replaces.empty() || !item.binaries.empty() || !item.tags.empty() ||
		!item.extraFields.empty();
}

CommandOutput package_table_output(ActionType action,
	                               const std::vector<Request>& requests,
	                               const std::vector<PackageInfo>& items) {
	CommandOutput output;
	output.mode = action == ActionType::LIST ? DisplayMode::LIST
	              : action == ActionType::SEARCH ? DisplayMode::SEARCH
	              : action == ActionType::OUTDATED ? DisplayMode::OUTDATED
	              : DisplayMode::LIST;
	for (const auto& request : requests) {
		output.sessionItems.push_back(request.system.empty() ? "all" : request.system);
	}
	const bool includeSystem = requests.size() > 1;
	std::vector<std::string> headers;
	if (includeSystem) {
		headers.push_back("System");
	}
	headers.push_back("Name");
	if (action == ActionType::OUTDATED) {
		headers.push_back("Installed");
		headers.push_back("Latest");
		headers.push_back("Type");
		headers.push_back("Architecture");
		headers.push_back("Target Systems");
		headers.push_back("Description");
		output.blocks.push_back(make_command_table_block(headers, package_outdated_infos_to_rows(items, includeSystem)));
	} else if (action == ActionType::SEARCH || action == ActionType::LIST) {
		headers.push_back("Version");
		headers.push_back("Type");
		headers.push_back("Architecture");
		headers.push_back("Target Systems");
		headers.push_back("Description");
		if (action == ActionType::SEARCH) {
			output.blocks.push_back(make_command_table_block(headers, package_search_infos_to_rows(items, includeSystem)));
		} else {
			output.blocks.push_back(make_command_table_block(headers, package_list_infos_to_rows(items, includeSystem)));
		}
	} else {
		headers.push_back("Version");
		headers.push_back("Summary");
		output.blocks.push_back(make_command_table_block(headers, package_list_infos_to_rows(items, includeSystem)));
	}
	if (items.empty()) {
		output.blocks.push_back(make_command_message_block("No results"));
	}
	output.success = true;
	output.succeeded = static_cast<int>(items.size());
	return output;
}

CommandOutput package_info_output(const Request& request, PackageInfo item) {
	CommandOutput output;
	output.mode = DisplayMode::INFO;
	output.sessionItems = {request.system.empty() ? "info" : request.system};
	if (!package_info_has_details(item)) {
		output.success = false;
		output.failed = 1;
		output.blocks.push_back(make_command_message_block("No package info found"));
		return output;
	}
	if (item.system.empty()) {
		item.system = request.system;
	}
	if (item.name.empty() && !request.packages.empty()) {
		item.name = request.packages.front();
	}
	const auto fields = package_info_to_fields(item);
	output.blocks.push_back(make_command_field_value_block(fields));
	output.success = true;
	output.succeeded = 1;
	return output;
}

std::vector<PackageInfo> collect_list_items(const std::vector<Request>& requests, Executer* executor) {
	std::vector<PackageInfo> items;
	for (const Request& request : requests) {
		auto listed = executor->list(request);
		for (auto& item : listed) {
			if (item.system.empty()) {
				item.system = request.system;
			}
			items.push_back(std::move(item));
		}
	}
	return items;
}

std::vector<PackageInfo> collect_outdated_items(const std::vector<Request>& requests, Executer* executor) {
	std::vector<PackageInfo> items;
	for (const Request& request : requests) {
		auto outdated = executor->outdated(request);
		for (auto& item : outdated) {
			if (item.system.empty()) {
				item.system = request.system;
			}
			items.push_back(std::move(item));
		}
	}
	return items;
}

std::vector<PackageInfo> collect_search_items(const std::vector<Request>& requests, Executer* executor) {
	std::vector<PackageInfo> items;
	for (const Request& request : requests) {
		auto searched = executor->search(request);
		for (auto& item : searched) {
			if (item.system.empty()) {
				item.system = request.system;
			}
			items.push_back(std::move(item));
		}
	}
	return items;
}

} // namespace

namespace orchestrator_internal {

int run_query_action(ActionType action, const std::vector<Request>& requests, Executer* executor) {
	if (action == ActionType::LIST) {
		render_command_output(package_table_output(ActionType::LIST, requests, collect_list_items(requests, executor)));
		return 0;
	}

	if (action == ActionType::OUTDATED) {
		render_command_output(package_table_output(ActionType::OUTDATED, requests, collect_outdated_items(requests, executor)));
		return 0;
	}

	if (action == ActionType::SEARCH) {
		render_command_output(package_table_output(ActionType::SEARCH, requests, collect_search_items(requests, executor)));
		return 0;
	}

	const PackageInfo item = executor->info(requests.front());
	const CommandOutput output = package_info_output(requests.front(), item);
	render_command_output(output);
	return output.success ? 0 : 1;
}

} // namespace orchestrator_internal
