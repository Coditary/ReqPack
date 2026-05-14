#include "output/command_output.h"

namespace {

void append_field(std::vector<CommandOutputField>& fields, const std::string& key, const std::string& value) {
    if (!value.empty()) {
        fields.push_back(CommandOutputField{.key = key, .value = value});
    }
}

void append_list_field(
    std::vector<CommandOutputField>& fields,
    const std::string& key,
    const std::vector<std::string>& values
) {
    const std::string joined = join_command_output_values(values);
    if (!joined.empty()) {
        fields.push_back(CommandOutputField{.key = key, .value = joined});
    }
}

std::string package_row_description(const PackageInfo& item) {
    if (!item.summary.empty()) {
        return item.summary;
    }
    return item.description;
}

}  // namespace

std::vector<CommandOutputField> package_info_to_fields(const PackageInfo& info) {
    std::vector<CommandOutputField> fields;
    append_field(fields, "System", info.system);
    append_field(fields, "Name", info.name);
    append_field(fields, "Package ID", info.packageId);
    append_field(fields, "Version", info.version);
    append_field(fields, "Latest Version", info.latestVersion);
    append_field(fields, "Status", info.status);
    append_field(fields, "Installed", info.installed);
    append_field(fields, "Summary", info.summary);
    append_field(fields, "Description", info.description);
    append_field(fields, "Homepage", info.homepage);
    append_field(fields, "Documentation", info.documentation);
    append_field(fields, "Source URL", info.sourceUrl);
    append_field(fields, "Repository", info.repository);
    append_field(fields, "Channel", info.channel);
    append_field(fields, "Section", info.section);
    append_field(fields, "Type", info.packageType);
    append_field(fields, "Architecture", info.architecture);
    append_field(fields, "Target Systems", info.targetSystems);
    append_field(fields, "License", info.license);
    append_field(fields, "Author", info.author);
    append_field(fields, "Maintainer", info.maintainer);
    append_field(fields, "Email", info.email);
    append_field(fields, "Published", info.publishedAt);
    append_field(fields, "Updated", info.updatedAt);
    append_field(fields, "Size", info.size);
    append_field(fields, "Installed Size", info.installedSize);
    append_list_field(fields, "Dependencies", info.dependencies);
    append_list_field(fields, "Optional Dependencies", info.optionalDependencies);
    append_list_field(fields, "Provides", info.provides);
    append_list_field(fields, "Conflicts", info.conflicts);
    append_list_field(fields, "Replaces", info.replaces);
    append_list_field(fields, "Binaries", info.binaries);
    append_list_field(fields, "Tags", info.tags);
    for (const auto& [key, value] : info.extraFields) {
        append_field(fields, key, value);
    }
    return fields;
}

std::vector<std::vector<std::string>> package_list_infos_to_rows(const std::vector<PackageInfo>& items, bool includeSystem) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        std::vector<std::string> row;
        if (includeSystem) {
            row.push_back(item.system);
        }
        row.push_back(item.name);
        row.push_back(item.version);
        row.push_back(item.packageType);
        row.push_back(item.architecture);
        row.push_back(item.targetSystems);
        row.push_back(package_row_description(item));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::vector<std::string>> package_search_infos_to_rows(const std::vector<PackageInfo>& items, bool includeSystem) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        std::vector<std::string> row;
        if (includeSystem) {
            row.push_back(item.system);
        }
        row.push_back(item.name);
        row.push_back(item.version);
        row.push_back(item.packageType);
        row.push_back(item.architecture);
        row.push_back(item.targetSystems);
        row.push_back(package_row_description(item));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::vector<std::string>> package_outdated_infos_to_rows(const std::vector<PackageInfo>& items, bool includeSystem) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        std::vector<std::string> row;
        if (includeSystem) {
            row.push_back(item.system);
        }
        row.push_back(item.name);
        row.push_back(item.version);
        row.push_back(item.latestVersion);
        row.push_back(item.packageType);
        row.push_back(item.architecture);
        row.push_back(item.targetSystems);
        row.push_back(package_row_description(item));
        rows.push_back(std::move(row));
    }
    return rows;
}
