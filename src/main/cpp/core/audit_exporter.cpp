#include "core/audit_exporter.h"

#include "output/ansi_color.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t TABLE_GAP_WIDTH = 1;
constexpr std::size_t TABLE_MIN_MESSAGE_WIDTH = 24;
constexpr std::size_t TABLE_WIDE_EXTRA_WIDTH = 40;

struct TableColumn {
    const char* header;
    std::size_t minWidth;
    std::size_t maxWidth;
};

constexpr std::array<TableColumn, 6> TABLE_COLUMNS{{
    {"SYSTEM", 6, 10},
    {"NAME", 12, 24},
    {"VERSION", 7, 12},
    {"FINDING", 12, 19},
    {"SEVERITY", 8, 10},
    {"SCORE", 5, 5},
}};

std::string normalize_table_value(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());

    bool inWhitespace = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            inWhitespace = !normalized.empty();
            continue;
        }
        if (inWhitespace) {
            normalized.push_back(' ');
            inWhitespace = false;
        }
        normalized.push_back(static_cast<char>(c));
    }
    return normalized;
}

std::size_t terminal_width() {
    if (const char* columns = std::getenv("COLUMNS")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(columns, &end, 10);
        if (end != columns && end != nullptr && *end == '\0' && parsed > 0) {
            return static_cast<std::size_t>(parsed);
        }
    }

    winsize size{};
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return static_cast<std::size_t>(size.ws_col);
    }

    return 120;
}

std::string fit_table_cell(const std::string& value, const std::size_t width) {
    const std::string normalized = normalize_table_value(value);
    if (normalized.size() <= width) {
        return normalized;
    }
    if (width <= 3) {
        return normalized.substr(0, width);
    }
    return normalized.substr(0, width - 3) + "...";
}

bool request_has_flag(const Request& request, const std::string& name) {
    return std::find(request.flags.begin(), request.flags.end(), name) != request.flags.end();
}

std::vector<std::string> wrap_table_text(const std::string& value, const std::size_t width) {
    if (width == 0) {
        return {normalize_table_value(value)};
    }

    const std::string normalized = normalize_table_value(value);
    if (normalized.empty()) {
        return {""};
    }

    std::vector<std::string> lines;
    std::string current;
    std::istringstream tokens(normalized);
    std::string token;

    auto flush_current = [&]() {
        if (!current.empty()) {
            lines.push_back(current);
            current.clear();
        }
    };

    while (tokens >> token) {
        while (token.size() > width) {
            if (!current.empty()) {
                flush_current();
            }
            lines.push_back(token.substr(0, width));
            token.erase(0, width);
        }

        if (current.empty()) {
            current = token;
            continue;
        }

        if (current.size() + 1 + token.size() <= width) {
            current += ' ';
            current += token;
            continue;
        }

        flush_current();
        current = token;
    }

    flush_current();
    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

std::array<std::size_t, TABLE_COLUMNS.size()> table_column_widths(
    const std::vector<std::array<std::string, TABLE_COLUMNS.size()>>& rows, const std::size_t width) {
    std::array<std::size_t, TABLE_COLUMNS.size()> widths{};
    std::size_t usedWidth = 0;
    for (std::size_t index = 0; index < TABLE_COLUMNS.size(); ++index) {
        widths[index] = TABLE_COLUMNS[index].minWidth;
        usedWidth += widths[index];
    }

    const std::size_t gapWidth = TABLE_GAP_WIDTH * TABLE_COLUMNS.size();
    std::size_t budget = 0;
    if (width > gapWidth + TABLE_MIN_MESSAGE_WIDTH) {
        budget = width - gapWidth - TABLE_MIN_MESSAGE_WIDTH;
    }
    if (budget <= usedWidth) {
        return widths;
    }

    std::size_t extra = budget - usedWidth;
    const std::array<std::size_t, TABLE_COLUMNS.size()> growthOrder{{3, 1, 2, 0, 4, 5}};
    for (const std::size_t index : growthOrder) {
        std::size_t desired = std::strlen(TABLE_COLUMNS[index].header);
        for (const auto& row : rows) {
            desired = std::max(desired, normalize_table_value(row[index]).size());
        }
        desired = std::min(desired, TABLE_COLUMNS[index].maxWidth);
        if (desired <= widths[index]) {
            continue;
        }

        const std::size_t growth = std::min(extra, desired - widths[index]);
        widths[index] += growth;
        extra -= growth;
        if (extra == 0) {
            break;
        }
    }

    return widths;
}

std::size_t table_message_width(
    const std::array<std::size_t, TABLE_COLUMNS.size()>& widths,
    const std::size_t terminalWidth,
    const bool wideTable
) {
    std::size_t usedWidth = TABLE_COLUMNS.size() * TABLE_GAP_WIDTH;
    for (const std::size_t width : widths) {
        usedWidth += width;
    }
    std::size_t availableWidth = terminalWidth;
    if (wideTable) {
        availableWidth += TABLE_WIDE_EXTRA_WIDTH;
    }
    if (availableWidth > usedWidth) {
        return availableWidth - usedWidth;
    }
    return TABLE_MIN_MESSAGE_WIDTH;
}

std::string padded_table_cell(const std::string& value, const std::size_t width) {
    std::ostringstream stream;
    stream << std::left << std::setw(static_cast<int>(width)) << fit_table_cell(value, width);
    return stream.str();
}

void append_table_cell(
    std::ostringstream& stream,
    const std::string& value,
    const std::size_t width,
    const std::string& colorSpec = {},
    const bool trailingGap = true
) {
    const std::string cell = padded_table_cell(value, width);
    if (colorSpec.empty()) {
        stream << cell;
    } else {
        stream << ansi_wrap(cell, colorSpec);
    }
    if (trailingGap) {
        stream << std::string(TABLE_GAP_WIDTH, ' ');
    }
}

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool force_color_enabled() {
    const char* value = std::getenv("FORCE_COLOR");
    if (value == nullptr) {
        return false;
    }

    const std::string normalized = to_lower_copy(value);
    return normalized.empty() || (normalized != "0" && normalized != "false" && normalized != "no");
}

bool table_colors_enabled() {
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    if (force_color_enabled()) {
        return true;
    }
    return isatty(STDOUT_FILENO);
}

std::string severity_color_spec_for(const ValidationFinding& finding) {
    const std::string severity = to_lower_copy(finding.severity);
    if (severity == "critical") {
        return "bold bright_red";
    }
    if (severity == "high") {
        return "bold red";
    }
    if (severity == "medium") {
        return "bold yellow";
    }
    if (severity == "low") {
        return "yellow";
    }
    if (severity == "unassigned") {
        return "bright_black";
    }
    return {};
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                escaped.push_back(static_cast<char>(c));
                break;
        }
    }
    return escaped;
}

std::vector<Graph::vertex_descriptor> ordered_vertices(const Graph& graph) {
    std::vector<Graph::vertex_descriptor> vertices;
    auto [it, end] = boost::vertices(graph);
    for (; it != end; ++it) {
        vertices.push_back(*it);
    }
    return vertices;
}

std::string package_component_ref(const Package& package) {
    std::ostringstream stream;
    stream << package.system << ':' << package.name;
    if (!package.version.empty()) {
        stream << '@' << package.version;
    }
    if (package.localTarget && !package.sourcePath.empty()) {
        stream << '#' << package.sourcePath;
    }
    return stream.str();
}

std::string package_display_name(const Package& package) {
    if (package.localTarget && !package.sourcePath.empty()) {
        return std::filesystem::path(package.sourcePath).filename().string();
    }
    return package.name;
}

std::string purl_name_for(const Package& package, const PluginSecurityMetadata& metadata) {
    if (package.name.empty()) {
        return {};
    }

    if (metadata.purlType == "maven") {
        const std::size_t separator = package.name.find(':');
        if (separator == std::string::npos || separator == 0 || separator == package.name.size() - 1) {
            return {};
        }
        return package.name.substr(0, separator) + "/" + package.name.substr(separator + 1);
    }

    return package.name;
}

std::string purl_for(const Package& package, PluginMetadataProvider* metadataProvider) {
    if (package.name.empty() || metadataProvider == nullptr) {
        return {};
    }

    const auto metadata = metadataProvider->getPluginSecurityMetadata(package.system);
    if (!metadata.has_value() || metadata->purlType.empty()) {
        return {};
    }

    const std::string purlName = purl_name_for(package, metadata.value());
    if (purlName.empty()) {
        return {};
    }

    std::string purl = "pkg:" + metadata->purlType + "/" + purlName;
    if (!package.version.empty()) {
        purl += '@' + package.version;
    }
    return purl;
}

std::string sarif_level_for(const ValidationFinding& finding) {
    const std::string severity = to_lower_copy(finding.severity);
    int severityRank = 0;
    if (severity == "critical") {
        severityRank = 4;
    } else if (severity == "high") {
        severityRank = 3;
    } else if (severity == "medium") {
        severityRank = 2;
    } else if (severity == "low") {
        severityRank = 1;
    }
    if (finding.kind == "sync_error") {
        return "error";
    }
    if (severityRank >= 3) {
        return "error";
    }
    if (severityRank >= 1) {
        return "warning";
    }
    return "note";
}

std::string rule_id_for(const ValidationFinding& finding) {
    if (!finding.id.empty()) {
        return finding.id;
    }
    return "reqpack-" + finding.kind;
}

std::string message_for(const ValidationFinding& finding) {
    if (!finding.message.empty()) {
        return finding.message;
    }
    if (!finding.id.empty()) {
        return finding.id;
    }
    return finding.kind;
}

std::string vex_analysis_detail_for(const ValidationFinding& finding) {
    if (finding.kind == "vulnerability") {
        return "Matched by ReqPack audit from local vulnerability data. Reachability and exploitability were not analyzed.";
    }
    if (finding.kind == "unresolved_version") {
        return "ReqPack could not resolve package version. Vulnerability matching may be incomplete.";
    }
    if (finding.kind == "unsupported_ecosystem") {
        return "ReqPack has no vulnerability ecosystem mapping for this package system.";
    }
    if (finding.kind == "sync_error") {
        return "ReqPack could not fully refresh vulnerability data during audit.";
    }
    return message_for(finding);
}

std::optional<Package> find_matching_package(const Graph& graph, const ValidationFinding& finding) {
    auto [it, end] = boost::vertices(graph);
    for (; it != end; ++it) {
        const Package& candidate = graph[*it];
        if (candidate.system == finding.package.system && candidate.name == finding.package.name &&
            candidate.version == finding.package.version && candidate.sourcePath == finding.package.sourcePath) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool write_output_file(const std::string& rendered, const Request& request, const std::string& outputPath) {
    std::filesystem::path filePath(outputPath);
    if (filePath.is_relative()) {
        filePath = std::filesystem::current_path() / filePath;
    }
    const std::string resolvedOutputPath = filePath.string();

    if (std::filesystem::exists(filePath)) {
        const bool force = std::find(request.flags.begin(), request.flags.end(), "force") != request.flags.end();
        if (!force) {
            std::cerr << resolvedOutputPath << " already exists. Overwrite? [y/N] " << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
                std::cerr << "aborted.\n";
                return false;
            }
        }
    }

    std::error_code error;
    const std::filesystem::path parentPath = filePath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, error);
        if (error) {
            std::cerr << "failed to create audit output directory: " << resolvedOutputPath << '\n';
            return false;
        }
    }

    std::ofstream output(resolvedOutputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "failed to open audit output path: " << resolvedOutputPath << '\n';
        return false;
    }

    output << rendered;
    if (!output.good()) {
        std::cerr << "failed to write audit output path: " << resolvedOutputPath << '\n';
        return false;
    }

    std::cout << resolvedOutputPath << '\n';
    std::cout.flush();
    return true;
}

}  // namespace

AuditExporter::AuditExporter(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
    : config(config), metadataProvider(metadataProvider) {}

AuditOutputFormat AuditExporter::resolveFormat(const Request& request) const {
    if (!request.outputFormat.empty()) {
        const auto parsed = audit_output_format_from_string(request.outputFormat);
        if (parsed.has_value()) {
            return parsed.value();
        }
    }

    if (!request.outputPath.empty()) {
        const std::string extension = to_lower_copy(std::filesystem::path(request.outputPath).extension().string());
        if (extension == ".sarif") {
            return AuditOutputFormat::SARIF;
        }
        return AuditOutputFormat::CYCLONEDX_VEX_JSON;
    }

    return AuditOutputFormat::TABLE;
}

std::string AuditExporter::resolveOutputPath(const Request& request) const {
    return request.outputPath;
}

std::string AuditExporter::renderTable(
    const Graph& graph,
    const std::vector<ValidationFinding>& findings,
    const bool colorizeSeverity,
    const bool disableWrap,
    const bool wideTable
) const {
    std::ostringstream stream;
    if (findings.empty()) {
        stream << "No vulnerabilities or audit findings detected.\n";
        return stream.str();
    }

    std::vector<std::array<std::string, TABLE_COLUMNS.size()>> rows;
    rows.reserve(findings.size());
    std::vector<std::string> messages;
    messages.reserve(findings.size());
    for (const ValidationFinding& finding : findings) {
        const std::optional<Package> matchedPackage = find_matching_package(graph, finding);
        const Package& resolved = matchedPackage.has_value() ? matchedPackage.value() : finding.package;

        std::ostringstream scoreStream;
        scoreStream << finding.score;

        rows.push_back({
            resolved.system,
            package_display_name(resolved),
            resolved.version.empty() ? "-" : resolved.version,
            finding.id.empty() ? finding.kind : finding.id,
            finding.severity.empty() ? "unassigned" : finding.severity,
            scoreStream.str(),
        });
        messages.push_back(message_for(finding));
    }

    const std::size_t terminalWidth = terminal_width();
    const std::array<std::size_t, TABLE_COLUMNS.size()> widths = table_column_widths(rows, terminalWidth);
    const std::size_t messageWidth = table_message_width(widths, terminalWidth, wideTable);

    for (std::size_t index = 0; index < TABLE_COLUMNS.size(); ++index) {
        append_table_cell(stream, TABLE_COLUMNS[index].header, widths[index]);
    }
    stream << "MESSAGE\n";

    for (std::size_t index = 0; index < TABLE_COLUMNS.size(); ++index) {
        append_table_cell(stream, std::string(widths[index], '-'), widths[index]);
    }
    stream << std::string(std::max<std::size_t>(7, messageWidth), '-') << '\n';

    for (std::size_t rowIndex = 0; rowIndex < findings.size(); ++rowIndex) {
        const auto messageLines = disableWrap ? std::vector<std::string>{normalize_table_value(messages[rowIndex])} : wrap_table_text(messages[rowIndex], messageWidth);
        const std::string severityColor = colorizeSeverity ? severity_color_spec_for(findings[rowIndex]) : std::string{};
        for (std::size_t lineIndex = 0; lineIndex < messageLines.size(); ++lineIndex) {
            for (std::size_t columnIndex = 0; columnIndex < TABLE_COLUMNS.size(); ++columnIndex) {
                const bool colorizeColumn = lineIndex == 0 && columnIndex == 4 && !severityColor.empty();
                append_table_cell(
                    stream,
                    lineIndex == 0 ? rows[rowIndex][columnIndex] : "",
                    widths[columnIndex],
                    colorizeColumn ? severityColor : std::string{}
                );
            }
            stream << messageLines[lineIndex] << '\n';
        }
    }
    return stream.str();
}

std::string AuditExporter::renderJson(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"summary\": {\n";
    stream << "    \"packageCount\": " << ordered_vertices(graph).size() << ",\n";
    stream << "    \"findingCount\": " << findings.size() << "\n";
    stream << "  },\n";
    stream << "  \"packages\": [\n";

    const std::vector<Graph::vertex_descriptor> vertices = ordered_vertices(graph);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const Package& package = graph[vertices[index]];
        stream << "    {\n";
        stream << "      \"system\": \"" << json_escape(package.system) << "\",\n";
        stream << "      \"name\": \"" << json_escape(package.name) << "\",\n";
        stream << "      \"displayName\": \"" << json_escape(package_display_name(package)) << "\",\n";
        stream << "      \"version\": \"" << json_escape(package.version) << "\",\n";
        stream << "      \"sourcePath\": \"" << json_escape(package.sourcePath) << "\",\n";
        stream << "      \"localTarget\": " << (package.localTarget ? "true" : "false") << ",\n";
        stream << "      \"ref\": \"" << json_escape(package_component_ref(package)) << "\"\n";
        stream << "    }";
        if (index + 1 < vertices.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ],\n";
    stream << "  \"findings\": [\n";
    for (std::size_t index = 0; index < findings.size(); ++index) {
        const ValidationFinding& finding = findings[index];
        stream << "    {\n";
        stream << "      \"id\": \"" << json_escape(finding.id) << "\",\n";
        stream << "      \"kind\": \"" << json_escape(finding.kind) << "\",\n";
        stream << "      \"source\": \"" << json_escape(finding.source) << "\",\n";
        stream << "      \"severity\": \"" << json_escape(finding.severity) << "\",\n";
        stream << "      \"score\": " << finding.score << ",\n";
        stream << "      \"message\": \"" << json_escape(message_for(finding)) << "\",\n";
        stream << "      \"package\": {\n";
        stream << "        \"system\": \"" << json_escape(finding.package.system) << "\",\n";
        stream << "        \"name\": \"" << json_escape(finding.package.name) << "\",\n";
        stream << "        \"version\": \"" << json_escape(finding.package.version) << "\"\n";
        stream << "      }\n";
        stream << "    }";
        if (index + 1 < findings.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]\n";
    stream << "}";
    return stream.str();
}

std::string AuditExporter::renderCycloneDxVex(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"bomFormat\": \"CycloneDX\",\n";
    stream << "  \"specVersion\": \"1.5\",\n";
    stream << "  \"version\": 1,\n";
    stream << "  \"components\": [\n";

    const std::vector<Graph::vertex_descriptor> vertices = ordered_vertices(graph);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const Package& package = graph[vertices[index]];
        stream << "    {\n";
        stream << "      \"type\": \"library\",\n";
        stream << "      \"bom-ref\": \"" << json_escape(package_component_ref(package)) << "\",\n";
        stream << "      \"name\": \"" << json_escape(package_display_name(package)) << "\"";
        if (!package.version.empty()) {
            stream << ",\n      \"version\": \"" << json_escape(package.version) << "\"";
        }
        const std::string purl = purl_for(package, this->metadataProvider);
        if (!purl.empty()) {
            stream << ",\n      \"purl\": \"" << json_escape(purl) << "\"";
        }
        stream << ",\n      \"properties\": [\n";
        stream << "        {\"name\": \"reqpack:system\", \"value\": \"" << json_escape(package.system) << "\"}";
        if (!package.sourcePath.empty()) {
            stream << ",\n        {\"name\": \"reqpack:sourcePath\", \"value\": \"" << json_escape(package.sourcePath) << "\"}";
        }
        stream << "\n      ]\n";
        stream << "    }";
        if (index + 1 < vertices.size()) {
            stream << ',';
        }
        stream << '\n';
    }

    stream << "  ]";
    if (this->config.sbom.includeDependencyEdges) {
        stream << ",\n  \"dependencies\": [\n";
        bool firstDependency = true;
        auto [it, end] = boost::vertices(graph);
        for (; it != end; ++it) {
            const Package& package = graph[*it];
            if (!firstDependency) {
                stream << ",\n";
            }
            firstDependency = false;
            stream << "    {\n";
            stream << "      \"ref\": \"" << json_escape(package_component_ref(package)) << "\",\n";
            stream << "      \"dependsOn\": [";
            auto [adjacentIt, adjacentEnd] = boost::adjacent_vertices(*it, graph);
            bool firstEdge = true;
            for (; adjacentIt != adjacentEnd; ++adjacentIt) {
                if (!firstEdge) {
                    stream << ", ";
                }
                firstEdge = false;
                stream << '"' << json_escape(package_component_ref(graph[*adjacentIt])) << '"';
            }
            stream << "]\n";
            stream << "    }";
        }
        stream << "\n  ]";
    }

    stream << ",\n  \"vulnerabilities\": [\n";
    for (std::size_t index = 0; index < findings.size(); ++index) {
        const ValidationFinding& finding = findings[index];
        const std::string findingId = finding.id.empty() ? ("reqpack-" + finding.kind + "-" + std::to_string(index + 1)) : finding.id;
        stream << "    {\n";
        stream << "      \"id\": \"" << json_escape(findingId) << "\",\n";
        stream << "      \"source\": {\"name\": \"" << json_escape(finding.source.empty() ? "reqpack" : finding.source) << "\"},\n";
        stream << "      \"ratings\": [{\"source\": {\"name\": \"" << json_escape(finding.source.empty() ? "reqpack" : finding.source) << "\"}, \"severity\": \"" << json_escape(finding.severity.empty() ? "unassigned" : finding.severity) << "\", \"score\": " << finding.score << "}],\n";
        stream << "      \"analysis\": {\"state\": \"in_triage\", \"detail\": \"" << json_escape(vex_analysis_detail_for(finding)) << "\"},\n";
        stream << "      \"description\": \"" << json_escape(message_for(finding)) << "\",\n";
        stream << "      \"affects\": [{\"ref\": \"" << json_escape(package_component_ref(finding.package)) << "\"}]\n";
        stream << "    }";
        if (index + 1 < findings.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]\n";
    stream << "}";
    return stream.str();
}

std::string AuditExporter::renderSarif(const Graph& graph, const std::vector<ValidationFinding>& findings) const {
    (void)graph;

    std::vector<std::string> orderedRuleIds;
    std::map<std::string, ValidationFinding> representativeFindings;
    for (const ValidationFinding& finding : findings) {
        const std::string ruleId = rule_id_for(finding);
        if (!representativeFindings.contains(ruleId)) {
            representativeFindings.emplace(ruleId, finding);
            orderedRuleIds.push_back(ruleId);
        }
    }

    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"version\": \"2.1.0\",\n";
    stream << "  \"$schema\": \"https://json.schemastore.org/sarif-2.1.0.json\",\n";
    stream << "  \"runs\": [\n";
    stream << "    {\n";
    stream << "      \"tool\": {\n";
    stream << "        \"driver\": {\n";
    stream << "          \"name\": \"ReqPack\",\n";
    stream << "          \"rules\": [\n";
    for (std::size_t index = 0; index < orderedRuleIds.size(); ++index) {
        const ValidationFinding& finding = representativeFindings.at(orderedRuleIds[index]);
        stream << "            {\n";
        stream << "              \"id\": \"" << json_escape(orderedRuleIds[index]) << "\",\n";
        stream << "              \"shortDescription\": {\"text\": \"" << json_escape(message_for(finding)) << "\"},\n";
        stream << "              \"properties\": {\"kind\": \"" << json_escape(finding.kind) << "\"}\n";
        stream << "            }";
        if (index + 1 < orderedRuleIds.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "          ]\n";
    stream << "        }\n";
    stream << "      },\n";
    stream << "      \"results\": [\n";
    for (std::size_t index = 0; index < findings.size(); ++index) {
        const ValidationFinding& finding = findings[index];
        stream << "        {\n";
        stream << "          \"ruleId\": \"" << json_escape(rule_id_for(finding)) << "\",\n";
        stream << "          \"level\": \"" << json_escape(sarif_level_for(finding)) << "\",\n";
        stream << "          \"message\": {\"text\": \"" << json_escape(message_for(finding)) << "\"},\n";
        stream << "          \"locations\": [{\n";
        stream << "            \"physicalLocation\": {\n";
        stream << "              \"artifactLocation\": {\"uri\": \"pkg:" << json_escape(package_component_ref(finding.package)) << "\"}\n";
        stream << "            }\n";
        stream << "          }],\n";
        stream << "          \"properties\": {\n";
        stream << "            \"system\": \"" << json_escape(finding.package.system) << "\",\n";
        stream << "            \"package\": \"" << json_escape(finding.package.name) << "\",\n";
        stream << "            \"version\": \"" << json_escape(finding.package.version) << "\",\n";
        stream << "            \"kind\": \"" << json_escape(finding.kind) << "\",\n";
        stream << "            \"score\": " << finding.score << "\n";
        stream << "          }\n";
        stream << "        }";
        if (index + 1 < findings.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "      ]\n";
    stream << "    }\n";
    stream << "  ]\n";
    stream << "}";
    return stream.str();
}

std::string AuditExporter::renderGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const {
    switch (resolveFormat(request)) {
        case AuditOutputFormat::JSON:
            return renderJson(graph, findings);
        case AuditOutputFormat::CYCLONEDX_VEX_JSON:
            return renderCycloneDxVex(graph, findings);
        case AuditOutputFormat::SARIF:
            return renderSarif(graph, findings);
        case AuditOutputFormat::TABLE:
        default:
            return renderTable(
                graph,
                findings,
                request.outputPath.empty() && table_colors_enabled(),
                request_has_flag(request, "no-wrap"),
                request_has_flag(request, "wide")
            );
    }
}

bool AuditExporter::exportGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const {
    const std::string rendered = renderGraph(graph, findings, request);
    const std::string outputPath = resolveOutputPath(request);
    if (outputPath.empty()) {
        std::cout << rendered;
        if (rendered.empty() || rendered.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
        return true;
    }

    return write_output_file(rendered, request, outputPath);
}
