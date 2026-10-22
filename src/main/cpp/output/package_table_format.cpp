#include "output/package_table_format.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

constexpr const char* kEllipsis = "\xe2\x80\xa6";

std::string to_lower_ascii(std::string value) {
	for (char& character : value) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

std::string basename_path(const std::string& path) {
	const size_t slash = path.find_last_of("/\\");
	if (slash == std::string::npos) {
		return path;
	}
	return path.substr(slash + 1);
}

bool ends_with(const std::string& text, const std::string& suffix) {
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string strip_boilerplate_prefix(std::string text) {
	static const std::vector<std::string> prefixes = {
	    "ReqPack Lua wrapper plugin for ",
	    "Thin ReqPack wrapper bundle for ",
	    "ReqPack wrapper plugin for ",
	    "ReqPack plugin for ",
	};
	for (const auto& prefix : prefixes) {
		if (text.rfind(prefix, 0) == 0) {
			text.erase(0, prefix.size());
			break;
		}
	}
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
		text.erase(text.begin());
	}
	return text;
}

std::string extract_alias_target(const PackageInfo& item) {
	static const std::string prefix = "Alias for ";
	const std::string source = !item.summary.empty() ? item.summary : item.description;
	if (source.rfind(prefix, 0) == 0) {
		return source.substr(prefix.size());
	}
	return {};
}

bool is_rqp_artifact_name(const std::string& name) {
	const std::string base = basename_path(name);
	return ends_with(to_lower_ascii(base), ".rqp");
}

std::string strip_rqp_version_suffix(const std::string& baseName) {
	static const std::regex pattern(R"(^(.+?)-\d+\.\d+(?:\.\d+)?(?:[-+][\w.]+)?-(?:linux|darwin|windows|win32|macos)-[\w.-]+$)",
	                                std::regex::icase);
	std::smatch match;
	if (std::regex_match(baseName, match, pattern) && match.size() > 1) {
		return match[1].str();
	}
	return baseName;
}

}  // namespace

std::string package_table_display_name(const std::string& rawName) {
	std::string name = basename_path(rawName);
	if (ends_with(to_lower_ascii(name), ".rqp")) {
		name.erase(name.size() - 4);
		name = strip_rqp_version_suffix(name);
	}
	return name;
}

std::string package_table_platform_label(const PackageInfo& item) {
	const std::string os = to_lower_ascii(item.targetSystems);
	const std::string arch = item.architecture;
	if (os.empty() && arch.empty()) {
		return "any";
	}
	if (os.empty()) {
		return arch;
	}
	if (arch.empty()) {
		return os;
	}
	return os + "/" + arch;
}

std::string package_table_type_badge(const std::string& packageType) {
	const std::string normalized = to_lower_ascii(packageType);
	if (normalized.empty()) {
		return "pkg";
	}
	if (normalized == "package-manager" || normalized == "packagemanager") {
		return "pkg-mgr";
	}
	if (normalized == "package" || normalized == "binary" || normalized == "app") {
		return "pkg";
	}
	if (normalized == "plugin") {
		return "plugin";
	}
	if (normalized == "alias") {
		return "alias";
	}
	if (normalized.size() <= 8) {
		return normalized;
	}
	return normalized.substr(0, 7) + kEllipsis;
}

std::string package_table_clean_description(const PackageInfo& item) {
	std::string text = !item.summary.empty() ? item.summary : item.description;
	if (text.rfind("Alias for ", 0) == 0) {
		return {};
	}
	return strip_boilerplate_prefix(std::move(text));
}

std::string package_table_format_versions(const std::vector<std::string>& versions) {
	if (versions.empty()) {
		return {};
	}
	std::vector<std::string> unique = versions;
	std::sort(unique.begin(), unique.end(), std::greater<>());
	unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
	if (unique.size() == 1) {
		return unique.front();
	}
	return unique.front() + " (+" + std::to_string(unique.size() - 1) + ")";
}

std::vector<PreparedListRow> prepare_list_table_rows(const std::vector<PackageInfo>& items, const bool groupBySystem) {
	std::set<std::string> canonicalNames;
	for (const auto& item : items) {
		if (!is_rqp_artifact_name(item.name)) {
			canonicalNames.insert(package_table_display_name(item.name));
		}
	}

	struct GroupKey {
		std::string system;
		std::string name;
		std::string typeBadge;
		std::string platform;

		bool operator==(const GroupKey& other) const {
			return system == other.system && name == other.name && typeBadge == other.typeBadge
			    && platform == other.platform;
		}
	};

	struct GroupKeyHash {
		size_t operator()(const GroupKey& key) const {
			return std::hash<std::string>{}(key.system + '\0' + key.name + '\0' + key.typeBadge + '\0' + key.platform);
		}
	};

	struct GroupValue {
		std::vector<std::string> versions;
		std::string description;
		std::string aliasTarget;
	};

	std::unordered_map<GroupKey, GroupValue, GroupKeyHash> groups;

	for (const auto& item : items) {
		const std::string displayName = package_table_display_name(item.name);
		if (is_rqp_artifact_name(item.name) && canonicalNames.count(displayName) > 0) {
			continue;
		}

		const GroupKey key {
		    .system = groupBySystem ? item.system : std::string{},
		    .name = displayName,
		    .typeBadge = package_table_type_badge(item.packageType),
		    .platform = package_table_platform_label(item),
		};

		auto& group = groups[key];
		if (!item.version.empty()) {
			group.versions.push_back(item.version);
		}
		const std::string cleaned = package_table_clean_description(item);
		if (!cleaned.empty() && group.description.empty()) {
			group.description = cleaned;
		}
		if (key.typeBadge == "alias" && group.aliasTarget.empty()) {
			group.aliasTarget = extract_alias_target(item);
		}
	}

	std::vector<PreparedListRow> rows;
	rows.reserve(groups.size());
	for (const auto& [key, value] : groups) {
		rows.push_back(PreparedListRow {
		    .system = key.system,
		    .name = key.name,
		    .version = package_table_format_versions(value.versions),
		    .typeBadge = key.typeBadge,
		    .platform = key.platform,
		    .description = value.description,
		    .aliasTarget = value.aliasTarget,
		});
	}

	std::sort(rows.begin(), rows.end(), [](const PreparedListRow& left, const PreparedListRow& right) {
		if (left.name != right.name) {
			return left.name < right.name;
		}
		if (left.typeBadge != right.typeBadge) {
			return left.typeBadge < right.typeBadge;
		}
		return left.platform < right.platform;
	});
	return rows;
}

std::vector<std::vector<std::string>> prepared_list_rows_to_table_cells(const std::vector<PreparedListRow>& rows,
                                                                        const bool includeSystem,
                                                                        const std::string& system) {
	std::vector<std::vector<std::string>> tableRows;
	tableRows.reserve(rows.size());
	for (const auto& row : rows) {
		std::vector<std::string> cells;
		if (includeSystem) {
			cells.push_back(!row.system.empty() ? row.system : system);
		}
		cells.push_back(row.name);
		cells.push_back(row.version);
		cells.push_back(row.typeBadge);
		cells.push_back(row.platform);
		cells.push_back(row.description);
		tableRows.push_back(std::move(cells));
	}
	return tableRows;
}

namespace {

enum class ListSectionKind { Packages, Plugins, Aliases };

ListSectionKind section_for_badge(const std::string& badge) {
	if (badge == "alias") {
		return ListSectionKind::Aliases;
	}
	if (badge == "plugin" || badge == "pkg-mgr") {
		return ListSectionKind::Plugins;
	}
	return ListSectionKind::Packages;
}

std::string section_heading(const ListSectionKind kind, const size_t count) {
	switch (kind) {
		case ListSectionKind::Packages:
			return "Packages & Binaries (" + std::to_string(count) + ")";
		case ListSectionKind::Plugins:
			return "Package Manager Plugins (" + std::to_string(count) + ")";
		case ListSectionKind::Aliases:
			return "Aliases (" + std::to_string(count) + ")";
	}
	return {};
}

std::string section_emoji(const ListSectionKind kind) {
	switch (kind) {
		case ListSectionKind::Packages:
			return "📦 ";
		case ListSectionKind::Plugins:
			return "🔌 ";
		case ListSectionKind::Aliases:
			return "🔗 ";
	}
	return {};
}

}  // namespace

std::string format_grouped_list_output(const std::vector<PackageInfo>& items) {
	const std::vector<PreparedListRow> rows = prepare_list_table_rows(items);
	std::vector<PreparedListRow> packages;
	std::vector<PreparedListRow> plugins;
	std::vector<PreparedListRow> aliases;
	for (const auto& row : rows) {
		switch (section_for_badge(row.typeBadge)) {
			case ListSectionKind::Packages:
				packages.push_back(row);
				break;
			case ListSectionKind::Plugins:
				plugins.push_back(row);
				break;
			case ListSectionKind::Aliases:
				aliases.push_back(row);
				break;
		}
	}

	std::ostringstream output;
	const auto render_section = [&](const ListSectionKind kind, const std::vector<PreparedListRow>& sectionRows) {
		if (sectionRows.empty()) {
			return;
		}
		output << section_emoji(kind) << section_heading(kind, sectionRows.size()) << '\n';
		if (kind == ListSectionKind::Aliases) {
			std::ostringstream line;
			for (size_t index = 0; index < sectionRows.size(); ++index) {
				const auto& row = sectionRows[index];
				const std::string target = row.aliasTarget.empty() ? "?" : row.aliasTarget;
				if (index > 0) {
					line << "    ";
				}
				line << row.name << " \xe2\x86\x92 " << target;
			}
			output << "  " << line.str() << '\n';
			return;
		}

		for (const auto& row : sectionRows) {
			output << "  " << row.name;
			if (!row.version.empty()) {
				output << "      " << row.version;
			}
			if (row.platform != "any") {
				output << "  [" << row.platform << "]";
			}
			if (!row.description.empty()) {
				output << "  " << row.description;
			}
			output << '\n';
		}
	};

	render_section(ListSectionKind::Packages, packages);
	if (!packages.empty() && (!plugins.empty() || !aliases.empty())) {
		output << '\n';
	}
	render_section(ListSectionKind::Plugins, plugins);
	if (!plugins.empty() && !aliases.empty()) {
		output << '\n';
	}
	render_section(ListSectionKind::Aliases, aliases);
	return output.str();
}

bool package_table_platform_column_is_redundant(const std::vector<PackageInfo>& items,
                                                const std::string& hostPlatformLabel) {
	if (items.empty()) {
		return true;
	}
	for (const PackageInfo& item : items) {
		const std::string platform = package_table_platform_label(item);
		if (platform != "any" && platform != hostPlatformLabel) {
			return false;
		}
	}
	return true;
}

std::string package_table_format_search_description(const PackageInfo& item) {
	const std::string badge = package_table_type_badge(item.packageType);
	const std::string cleaned = package_table_clean_description(item);
	if (badge == "alias") {
		const std::string target = extract_alias_target(item);
		if (!target.empty()) {
			if (cleaned.empty()) {
				return "-> " + target;
			}
			return "-> " + target + " (" + cleaned + ")";
		}
	}
	return cleaned;
}

std::vector<PreparedSearchRow> prepare_search_table_rows(const std::vector<PackageInfo>& items,
                                                         const bool groupBySystem) {
	std::unordered_map<std::string, int> nameCollisionCounts;
	for (const PackageInfo& item : items) {
		const std::string displayName = package_table_display_name(item.name);
		++nameCollisionCounts[displayName];
	}

	std::vector<PreparedSearchRow> rows;
	rows.reserve(items.size());
	for (const PackageInfo& item : items) {
		const std::string displayName = package_table_display_name(item.name);
		const std::string badge = package_table_type_badge(item.packageType);
		std::string nameCell = displayName;
		if (nameCollisionCounts[displayName] > 1 && badge != "pkg") {
			nameCell = badge + "/" + displayName;
		}

		PreparedSearchRow row {
		    .system = groupBySystem ? item.system : std::string{},
		    .name = std::move(nameCell),
		    .version = item.version,
		    .typeBadge = badge,
		    .platform = package_table_platform_label(item),
		    .description = package_table_format_search_description(item),
		    .installed = item.installed == "true" || item.installed == "yes" || item.status == "installed",
		};
		if (row.installed) {
			if (row.description.empty()) {
				row.description = "[installed]";
			} else if (row.description.find("[installed]") == std::string::npos) {
				row.description += " [installed]";
			}
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

std::vector<std::vector<std::string>> prepared_search_rows_to_table_cells(const std::vector<PreparedSearchRow>& rows,
                                                                          const PackageSearchTableOptions& options) {
	std::vector<std::vector<std::string>> tableRows;
	tableRows.reserve(rows.size());
	for (const PreparedSearchRow& row : rows) {
		std::vector<std::string> cells;
		if (options.includeSystem) {
			cells.push_back(row.system);
		}
		std::string nameCell = row.name;
		if (row.installed) {
			nameCell = std::string("\xe2\x97\x8f ") + nameCell;
		}
		cells.push_back(std::move(nameCell));
		cells.push_back(row.version);
		cells.push_back(row.typeBadge);
		if (options.showPlatform) {
			cells.push_back(row.platform);
		}
		cells.push_back(row.description);
		tableRows.push_back(std::move(cells));
	}
	return tableRows;
}
