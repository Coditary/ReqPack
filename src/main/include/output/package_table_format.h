#pragma once

#include "core/common/types.h"

#include <string>
#include <vector>

struct PreparedListRow {
	std::string system;
	std::string name;
	std::string version;
	std::string typeBadge;
	std::string platform;
	std::string description;
	std::string aliasTarget;
};

struct PreparedSearchRow {
	std::string system;
	std::string name;
	std::string version;
	std::string typeBadge;
	std::string platform;
	std::string description;
	bool installed {false};
};

struct PackageSearchTableOptions {
	bool includeSystem {false};
	bool showPlatform {true};
};

std::string package_table_display_name(const std::string& rawName);
std::string package_table_platform_label(const PackageInfo& item);
std::string package_table_type_badge(const std::string& packageType);
std::string package_table_clean_description(const PackageInfo& item);
std::string package_table_format_versions(const std::vector<std::string>& versions);

std::vector<PreparedListRow> prepare_list_table_rows(const std::vector<PackageInfo>& items, bool groupBySystem = false);
std::vector<std::vector<std::string>> prepared_list_rows_to_table_cells(const std::vector<PreparedListRow>& rows,
                                                                        bool includeSystem,
                                                                        const std::string& system = {});

std::string format_grouped_list_output(const std::vector<PackageInfo>& items);

bool package_table_platform_column_is_redundant(const std::vector<PackageInfo>& items,
                                                const std::string& hostPlatformLabel);
std::vector<PreparedSearchRow> prepare_search_table_rows(const std::vector<PackageInfo>& items, bool groupBySystem);
std::vector<std::vector<std::string>> prepared_search_rows_to_table_cells(const std::vector<PreparedSearchRow>& rows,
                                                                          const PackageSearchTableOptions& options);
