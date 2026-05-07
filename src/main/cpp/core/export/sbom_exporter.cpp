#include "core/export/sbom_exporter.h"

#include "output/ansi_color.h"
#include "output/logger.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t TABLE_GAP_WIDTH = 1;
constexpr std::size_t TABLE_MIN_SOURCE_WIDTH = 24;
constexpr std::size_t TABLE_WIDE_EXTRA_WIDTH = 40;

struct TableColumn {
    const char* header;
    std::size_t minWidth;
    std::size_t maxWidth;
};

constexpr std::array<TableColumn, 3> TABLE_COLUMNS{{
    {"SYSTEM", 6, 10},
    {"NAME", 12, 28},
    {"VERSION", 7, 16},
}};

DiagnosticMessage sbom_output_diagnostic(const std::string& summary, const std::string& cause, const std::string& recommendation) {
    return make_error_diagnostic(
        "sbom",
        summary,
        cause,
        recommendation,
        {},
        "sbom",
        "output"
    );
}

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

std::string system_color_spec_for(const std::string& system) {
    const std::string normalized = to_lower_copy(system);
    if (normalized == "npm") {
        return "bold red";
    }
    if (normalized == "maven") {
        return "bold bright_red";
    }
    if (normalized == "pip") {
        return "bold bright_blue";
    }
    if (normalized == "rqp") {
        return "bold bright_magenta";
    }
    if (normalized == "apt" || normalized == "dnf" || normalized == "pacman" || normalized == "zypper") {
        return "bold green";
    }
    return "bold cyan";
}

std::string source_color_spec_for(const std::string& source) {
    return source == "-" ? "bright_black" : "bright_black";
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
    if (width > gapWidth + TABLE_MIN_SOURCE_WIDTH) {
        budget = width - gapWidth - TABLE_MIN_SOURCE_WIDTH;
    }
    if (budget <= usedWidth) {
        return widths;
    }

    std::size_t extra = budget - usedWidth;
    const std::array<std::size_t, TABLE_COLUMNS.size()> growthOrder{{1, 2, 0}};
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

std::size_t table_source_width(
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
    return TABLE_MIN_SOURCE_WIDTH;
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

std::string sbom_component_ref(const Package& package) {
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

std::vector<Graph::vertex_descriptor> ordered_vertices(const Graph& graph) {
    std::vector<Graph::vertex_descriptor> vertices;
    auto [it, end] = boost::vertices(graph);
    for (; it != end; ++it) {
        vertices.push_back(*it);
    }
    return vertices;
}

std::string package_display_name(const Package& package) {
    if (package.localTarget && !package.sourcePath.empty()) {
        return std::filesystem::path(package.sourcePath).filename().string();
    }
    return package.name;
}

}  // namespace

SbomExporter::SbomExporter(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
    : config(config), metadataProvider(metadataProvider) {}

SbomOutputFormat SbomExporter::resolveFormat(const Request& request) const {
    if (!request.outputFormat.empty()) {
        const auto parsed = sbom_output_format_from_string(request.outputFormat);
        if (parsed.has_value()) {
            return parsed.value();
        }
    }

    if (!request.outputPath.empty()) {
        return SbomOutputFormat::CYCLONEDX_JSON;
    }

    return this->config.sbom.defaultFormat;
}

std::string SbomExporter::resolveOutputPath(const Request& request) const {
    if (!request.outputPath.empty()) {
        return request.outputPath;
    }

    return this->config.sbom.defaultOutputPath;
}

std::string SbomExporter::renderTable(const Graph& graph, const bool colorizeTable, const bool disableWrap, const bool wideTable) const {
    std::ostringstream stream;
    std::vector<std::array<std::string, TABLE_COLUMNS.size()>> rows;
    rows.reserve(ordered_vertices(graph).size());
    std::vector<std::string> sources;
    sources.reserve(ordered_vertices(graph).size());

    const std::vector<Graph::vertex_descriptor> vertices = ordered_vertices(graph);
    for (const Graph::vertex_descriptor vertex : vertices) {
        const Package& package = graph[vertex];
        rows.push_back({
            package.system,
            package_display_name(package),
            package.version.empty() ? "-" : package.version,
        });
        sources.push_back(package.sourcePath.empty() ? "-" : package.sourcePath);
    }

    const std::size_t terminalWidth = terminal_width();
    const std::array<std::size_t, TABLE_COLUMNS.size()> widths = table_column_widths(rows, terminalWidth);
    const std::size_t sourceWidth = table_source_width(widths, terminalWidth, wideTable);

    for (std::size_t index = 0; index < TABLE_COLUMNS.size(); ++index) {
        append_table_cell(stream, TABLE_COLUMNS[index].header, widths[index]);
    }
    stream << "SOURCE\n";

    for (std::size_t index = 0; index < TABLE_COLUMNS.size(); ++index) {
        append_table_cell(stream, std::string(widths[index], '-'), widths[index]);
    }
    stream << std::string(std::max<std::size_t>(6, sourceWidth), '-') << '\n';

    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const auto sourceLines = disableWrap ? std::vector<std::string>{normalize_table_value(sources[rowIndex])} : wrap_table_text(sources[rowIndex], sourceWidth);
        const std::string systemColor = colorizeTable ? system_color_spec_for(rows[rowIndex][0]) : std::string{};
        const std::string sourceColor = colorizeTable ? source_color_spec_for(sources[rowIndex]) : std::string{};
        for (std::size_t lineIndex = 0; lineIndex < sourceLines.size(); ++lineIndex) {
            for (std::size_t columnIndex = 0; columnIndex < TABLE_COLUMNS.size(); ++columnIndex) {
                const bool colorizeColumn = lineIndex == 0 && columnIndex == 0 && !systemColor.empty();
                append_table_cell(
                    stream,
                    lineIndex == 0 ? rows[rowIndex][columnIndex] : "",
                    widths[columnIndex],
                    colorizeColumn ? systemColor : std::string{}
                );
            }
            if (lineIndex == 0 && !sourceColor.empty()) {
                stream << ansi_wrap(sourceLines[lineIndex], sourceColor) << '\n';
            } else {
                stream << sourceLines[lineIndex] << '\n';
            }
        }
    }
    return stream.str();
}

std::string SbomExporter::renderJson(const Graph& graph, const bool cyclonedx) const {
    std::ostringstream stream;
    if (cyclonedx) {
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
            stream << "      \"bom-ref\": \"" << json_escape(sbom_component_ref(package)) << "\",\n";
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
            if (package.localTarget) {
                stream << ",\n        {\"name\": \"reqpack:localTarget\", \"value\": \"true\"}";
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
                stream << "      \"ref\": \"" << json_escape(sbom_component_ref(package)) << "\",\n";
                stream << "      \"dependsOn\": [";
                auto [adjacentIt, adjacentEnd] = boost::adjacent_vertices(*it, graph);
                bool firstEdge = true;
                for (; adjacentIt != adjacentEnd; ++adjacentIt) {
                    if (!firstEdge) {
                        stream << ", ";
                    }
                    firstEdge = false;
                    stream << '\"' << json_escape(sbom_component_ref(graph[*adjacentIt])) << '\"';
                }
                stream << "]\n";
                stream << "    }";
            }
            stream << "\n  ]";
        }
        stream << "\n}";
        return stream.str();
    }

    stream << "{\n";
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
        stream << "      \"localTarget\": " << (package.localTarget ? "true" : "false") << "\n";
        stream << "    }";
        if (index + 1 < vertices.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]";
    if (this->config.sbom.includeDependencyEdges) {
        stream << ",\n  \"dependencies\": [\n";
        bool firstEdge = true;
        auto [edgeIt, edgeEnd] = boost::edges(graph);
        for (; edgeIt != edgeEnd; ++edgeIt) {
            if (!firstEdge) {
                stream << ",\n";
            }
            firstEdge = false;
            const Package& from = graph[boost::source(*edgeIt, graph)];
            const Package& to = graph[boost::target(*edgeIt, graph)];
            stream << "    {\"from\": \"" << json_escape(sbom_component_ref(from))
                   << "\", \"to\": \"" << json_escape(sbom_component_ref(to)) << "\"}";
        }
        stream << "\n  ]";
    }
    stream << "\n}";
    return stream.str();
}

std::string SbomExporter::renderGraph(const Graph& graph, const Request& request) const {
    switch (resolveFormat(request)) {
        case SbomOutputFormat::JSON:
            return renderJson(graph, false);
        case SbomOutputFormat::CYCLONEDX_JSON:
            return renderJson(graph, true);
        case SbomOutputFormat::TABLE:
        default:
            return renderTable(
                graph,
                request.outputPath.empty() && table_colors_enabled(),
                request_has_flag(request, "no-wrap"),
                request_has_flag(request, "wide")
            );
    }
}

bool SbomExporter::exportGraph(const Graph& graph, const Request& request) const {
    const std::string rendered = renderGraph(graph, request);
    const std::string outputPath = resolveOutputPath(request);
    if (outputPath.empty()) {
        std::cout << rendered;
        if (rendered.empty() || rendered.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
        return true;
    }

    std::filesystem::path filePath(outputPath);
    if (filePath.is_relative()) {
        filePath = std::filesystem::current_path() / filePath;
    }
    const std::string resolvedOutputPath = filePath.string();

    if (std::filesystem::exists(filePath)) {
        const bool force = std::find(request.flags.begin(), request.flags.end(), "force") != request.flags.end();
        if (!force) {
			Logger& logger = Logger::instance();
			logger.stdout(resolvedOutputPath + " already exists. Overwrite? [y/N]");
			logger.flushSync();
            std::string answer;
            if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
				logger.stdout("aborted.");
				logger.flushSync();
                return false;
            }
        }
    }

    std::error_code error;
    const std::filesystem::path parentPath = filePath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, error);
        if (error) {
			Logger::instance().diagnostic(sbom_output_diagnostic(
				"failed to create sbom output directory: " + resolvedOutputPath,
				"ReqPack could not create parent directory for SBOM export output.",
				"Check target path permissions and parent directory state, then retry."
			));
			Logger::instance().flushSync();
            return false;
        }
    }

    std::ofstream output(resolvedOutputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
		Logger::instance().diagnostic(sbom_output_diagnostic(
			"failed to open sbom output path: " + resolvedOutputPath,
			"ReqPack could not open requested SBOM output file for writing.",
			"Check whether path points to writable file location and retry."
		));
		Logger::instance().flushSync();
        return false;
    }

    output << rendered;
    if (!output.good()) {
		Logger::instance().diagnostic(sbom_output_diagnostic(
			"failed to write sbom output path: " + resolvedOutputPath,
			"ReqPack could not finish writing SBOM export to output file.",
			"Check disk space, filesystem health, and write permissions, then retry."
		));
		Logger::instance().flushSync();
        return false;
    }

    std::cout << resolvedOutputPath << '\n';
    std::cout.flush();
    return true;
}
