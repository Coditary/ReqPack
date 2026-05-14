#include "audit_exporter_internal.h"

#include "output/ansi_color.h"

#include <boost/graph/graph_traits.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <iomanip>
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
    const std::vector<std::array<std::string, TABLE_COLUMNS.size()>>& rows,
    const std::size_t width
) {
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

bool force_color_enabled() {
    const char* value = std::getenv("FORCE_COLOR");
    if (value == nullptr) {
        return false;
    }

    const std::string normalized = audit_exporter_internal::to_lower_copy(value);
    return normalized.empty() || (normalized != "0" && normalized != "false" && normalized != "no");
}

std::string severity_color_spec_for(const ValidationFinding& finding) {
    const std::string severity = audit_exporter_internal::to_lower_copy(finding.severity);
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

std::string message_for(const ValidationFinding& finding) {
    if (!finding.message.empty()) {
        return finding.message;
    }
    if (!finding.id.empty()) {
        return finding.id;
    }
    return finding.kind;
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

}  // namespace

namespace audit_exporter_internal {

bool table_colors_enabled() {
    if (std::getenv("NO_COLOR") != nullptr) {
        return false;
    }
    if (force_color_enabled()) {
        return true;
    }
    return isatty(STDOUT_FILENO);
}

}  // namespace audit_exporter_internal

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
            audit_exporter_internal::package_display_name(resolved),
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
        const auto messageLines = disableWrap
            ? std::vector<std::string>{normalize_table_value(messages[rowIndex])}
            : wrap_table_text(messages[rowIndex], messageWidth);
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
