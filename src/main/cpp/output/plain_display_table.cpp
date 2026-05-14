#include "output/plain_display.h"

#include "plain_display_internal.h"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>

std::vector<std::string> PlainDisplay::wrapText(const std::string& text, size_t width) const {
    if (text.empty()) {
        return {std::string{}};
    }
    if (width == 0) {
        return {text};
    }

    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string paragraph;
    while (std::getline(input, paragraph)) {
        std::istringstream words(paragraph);
        std::string word;
        std::string current;
        while (words >> word) {
            if (current.empty()) {
                if (word.size() <= width) {
                    current = word;
                } else {
                    const std::vector<std::string> segments = plain_display_internal::split_long_token(word, width);
                    for (size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
                        if (segmentIndex + 1 < segments.size()) {
                            lines.push_back(segments[segmentIndex]);
                        } else {
                            current = segments[segmentIndex];
                        }
                    }
                }
                continue;
            }

            if (current.size() + 1 + word.size() <= width) {
                current += " " + word;
                continue;
            }

            lines.push_back(current);
            if (word.size() <= width) {
                current = word;
            } else {
                current.clear();
                const std::vector<std::string> segments = plain_display_internal::split_long_token(word, width);
                for (size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
                    if (segmentIndex + 1 < segments.size()) {
                        lines.push_back(segments[segmentIndex]);
                    } else {
                        current = segments[segmentIndex];
                    }
                }
            }
        }

        if (!current.empty()) {
            lines.push_back(current);
        } else if (paragraph.empty()) {
            lines.push_back(std::string{});
        }
    }

    if (lines.empty()) {
        lines.push_back(std::string{});
    }
    return lines;
}

bool PlainDisplay::isPackageSummaryTable() const {
    return (tableHeaders.size() == 3 && tableHeaders[0] == "Name" && tableHeaders[1] == "Version" && tableHeaders[2] == "Summary")
        || (tableHeaders.size() == 5 && tableHeaders[0] == "Name" && tableHeaders[1] == "Version" && tableHeaders[2] == "Type"
            && tableHeaders[3] == "Architecture" && tableHeaders[4] == "Description")
        || (tableHeaders.size() == 6 && tableHeaders[0] == "Name" && tableHeaders[1] == "Version" && tableHeaders[2] == "Type"
            && tableHeaders[3] == "Architecture" && tableHeaders[4] == "Target Systems" && tableHeaders[5] == "Description")
        || (tableHeaders.size() == 6 && tableHeaders[0] == "Name" && tableHeaders[1] == "Installed" && tableHeaders[2] == "Latest"
            && tableHeaders[3] == "Type" && tableHeaders[4] == "Architecture" && tableHeaders[5] == "Description")
        || (tableHeaders.size() == 7 && tableHeaders[0] == "Name" && tableHeaders[1] == "Installed" && tableHeaders[2] == "Latest"
            && tableHeaders[3] == "Type" && tableHeaders[4] == "Architecture" && tableHeaders[5] == "Target Systems"
            && tableHeaders[6] == "Description")
        || (tableHeaders.size() == 4 && tableHeaders[0] == "System" && tableHeaders[1] == "Name" && tableHeaders[2] == "Version"
            && tableHeaders[3] == "Summary")
        || (tableHeaders.size() == 6 && tableHeaders[0] == "System" && tableHeaders[1] == "Name" && tableHeaders[2] == "Version"
            && tableHeaders[3] == "Type" && tableHeaders[4] == "Architecture" && tableHeaders[5] == "Description")
        || (tableHeaders.size() == 7 && tableHeaders[0] == "System" && tableHeaders[1] == "Name" && tableHeaders[2] == "Version"
            && tableHeaders[3] == "Type" && tableHeaders[4] == "Architecture" && tableHeaders[5] == "Target Systems"
            && tableHeaders[6] == "Description")
        || (tableHeaders.size() == 7 && tableHeaders[0] == "System" && tableHeaders[1] == "Name" && tableHeaders[2] == "Installed"
            && tableHeaders[3] == "Latest" && tableHeaders[4] == "Type" && tableHeaders[5] == "Architecture"
            && tableHeaders[6] == "Description")
        || (tableHeaders.size() == 8 && tableHeaders[0] == "System" && tableHeaders[1] == "Name" && tableHeaders[2] == "Installed"
            && tableHeaders[3] == "Latest" && tableHeaders[4] == "Type" && tableHeaders[5] == "Architecture"
            && tableHeaders[6] == "Target Systems" && tableHeaders[7] == "Description");
}

void PlainDisplay::renderFieldValueTable() const {
    const size_t terminalWidth = plain_display_internal::terminal_width();
    const size_t keyWidth = std::min<size_t>(std::max<size_t>(colWidths.empty() ? 12 : colWidths[0], 12), 22);
    const size_t valueIndent = keyWidth + 2;
    const size_t valueWidth = terminalWidth > valueIndent ? terminalWidth - valueIndent : 40;

    out() << '\n';
    for (const auto& row : tableRows) {
        const std::string key = row.empty() ? std::string{} : row[0];
        const std::string value = row.size() > 1 ? row[1] : std::string{};
        const std::vector<std::string> wrapped = wrapText(value, valueWidth);
        for (size_t lineIndex = 0; lineIndex < wrapped.size(); ++lineIndex) {
            if (lineIndex == 0) {
                out() << std::left << std::setw(static_cast<int>(keyWidth)) << key << ": ";
            } else {
                out() << std::string(valueIndent, ' ');
            }
            out() << wrapped[lineIndex] << '\n';
        }
    }
    out() << '\n';
    out().flush();
}

void PlainDisplay::renderWrappedPackageTable() const {
    const size_t terminalWidth = plain_display_internal::terminal_width();
    const bool hasSystem = !tableHeaders.empty() && tableHeaders[0] == "System";
    const bool outdated = std::find(tableHeaders.begin(), tableHeaders.end(), "Installed") != tableHeaders.end()
        && std::find(tableHeaders.begin(), tableHeaders.end(), "Latest") != tableHeaders.end();
    const bool extended = std::find(tableHeaders.begin(), tableHeaders.end(), "Type") != tableHeaders.end()
        && std::find(tableHeaders.begin(), tableHeaders.end(), "Architecture") != tableHeaders.end();
    const bool hasTargetSystems = std::find(tableHeaders.begin(), tableHeaders.end(), "Target Systems") != tableHeaders.end();
    const size_t systemIndex = hasSystem ? 0 : static_cast<size_t>(-1);
    const size_t nameIndex = hasSystem ? 1 : 0;
    const size_t versionIndex = outdated ? static_cast<size_t>(-1) : (hasSystem ? 2 : 1);
    const size_t installedIndex = outdated ? (hasSystem ? 2 : 1) : static_cast<size_t>(-1);
    const size_t latestIndex = outdated ? (hasSystem ? 3 : 2) : static_cast<size_t>(-1);
    const size_t typeIndex = extended ? (outdated ? (hasSystem ? 4 : 3) : (hasSystem ? 3 : 2)) : static_cast<size_t>(-1);
    const size_t archIndex = extended ? (outdated ? (hasSystem ? 5 : 4) : (hasSystem ? 4 : 3)) : static_cast<size_t>(-1);
    const size_t targetSystemsIndex = hasTargetSystems
        ? (outdated ? (hasSystem ? 6 : 5) : (extended ? (hasSystem ? 5 : 4) : static_cast<size_t>(-1)))
        : static_cast<size_t>(-1);
    const size_t summaryIndex = outdated
        ? (hasSystem ? (hasTargetSystems ? 7 : 6) : (hasTargetSystems ? 6 : 5))
        : (extended ? (hasSystem ? (hasTargetSystems ? 6 : 5) : (hasTargetSystems ? 5 : 4)) : (hasSystem ? 3 : 2));

    std::vector<size_t> widths(tableHeaders.size(), 0);
    auto preferredWidthFor = [&](size_t index) {
        if (index == summaryIndex) {
            return static_cast<size_t>(0);
        }
        if (hasSystem && index == systemIndex) {
            size_t systemWidth = tableHeaders[index].size();
            for (const auto& row : tableRows) {
                if (index < row.size()) {
                    systemWidth = std::max(systemWidth, row[index].size());
                }
            }
            return std::clamp(systemWidth, static_cast<size_t>(6), static_cast<size_t>(12));
        }
        if (index == nameIndex) {
            return static_cast<size_t>(50);
        }
        if ((!outdated && index == versionIndex) || (outdated && (index == installedIndex || index == latestIndex))) {
            return static_cast<size_t>(16);
        }
        if (extended && index == typeIndex) {
            return static_cast<size_t>(14);
        }
        if (extended && index == archIndex) {
            return static_cast<size_t>(12);
        }
        if (hasTargetSystems && index == targetSystemsIndex) {
            return static_cast<size_t>(12);
        }
        return std::max(tableHeaders[index].size(), static_cast<size_t>(12));
    };

    auto minimumWidthFor = [&](size_t index) {
        if (hasSystem && index == systemIndex) {
            return static_cast<size_t>(6);
        }
        if (index == nameIndex) {
            return static_cast<size_t>(12);
        }
        if ((!outdated && index == versionIndex) || (outdated && (index == installedIndex || index == latestIndex))) {
            return static_cast<size_t>(8);
        }
        if (extended && index == typeIndex) {
            return static_cast<size_t>(6);
        }
        if (extended && index == archIndex) {
            return static_cast<size_t>(8);
        }
        if (hasTargetSystems && index == targetSystemsIndex) {
            return static_cast<size_t>(6);
        }
        return tableHeaders[index].size();
    };

    for (size_t i = 0; i < tableHeaders.size(); ++i) {
        if (i == summaryIndex) {
            continue;
        }
        widths[i] = std::max(tableHeaders[i].size(), preferredWidthFor(i));
    }

    const size_t separatorWidth = tableHeaders.size() > 1 ? (tableHeaders.size() - 1) * 2 : 0;
    const size_t availableWidth = terminalWidth > separatorWidth ? terminalWidth - separatorWidth : 0;
    const size_t preferredSummaryWidth = std::max(tableHeaders[summaryIndex].size(), static_cast<size_t>(20));

    auto usedNonSummaryWidth = [&]() {
        size_t used = 0;
        for (size_t i = 0; i < tableHeaders.size(); ++i) {
            if (i != summaryIndex) {
                used += widths[i];
            }
        }
        return used;
    };

    auto shrinkColumn = [&](size_t index, size_t& remaining) {
        if (index == static_cast<size_t>(-1) || remaining == 0) {
            return;
        }
        const size_t minWidth = minimumWidthFor(index);
        const size_t reducible = widths[index] > minWidth ? widths[index] - minWidth : 0;
        const size_t taken = std::min(reducible, remaining);
        widths[index] -= taken;
        remaining -= taken;
    };

    const size_t desiredSummaryWidth = availableWidth == 0 ? static_cast<size_t>(0) : std::min(preferredSummaryWidth, availableWidth);
    const size_t maxNonSummaryWidth = availableWidth > desiredSummaryWidth ? availableWidth - desiredSummaryWidth : 0;
    size_t remaining = usedNonSummaryWidth() > maxNonSummaryWidth ? usedNonSummaryWidth() - maxNonSummaryWidth : 0;
    if (hasTargetSystems) {
        shrinkColumn(targetSystemsIndex, remaining);
    }
    shrinkColumn(nameIndex, remaining);
    if (hasSystem) {
        shrinkColumn(systemIndex, remaining);
    }
    if (extended) {
        shrinkColumn(typeIndex, remaining);
        shrinkColumn(archIndex, remaining);
    }
    if (outdated) {
        shrinkColumn(installedIndex, remaining);
        shrinkColumn(latestIndex, remaining);
    } else {
        shrinkColumn(versionIndex, remaining);
    }

    size_t summaryWidth = availableWidth > usedNonSummaryWidth() ? availableWidth - usedNonSummaryWidth() : 0;
    const size_t minSummaryHeaderWidth = tableHeaders[summaryIndex].size();
    if (summaryWidth < minSummaryHeaderWidth) {
        remaining = minSummaryHeaderWidth - summaryWidth;
        if (hasTargetSystems) {
            shrinkColumn(targetSystemsIndex, remaining);
        }
        shrinkColumn(nameIndex, remaining);
        if (hasSystem) {
            shrinkColumn(systemIndex, remaining);
        }
        if (extended) {
            shrinkColumn(typeIndex, remaining);
            shrinkColumn(archIndex, remaining);
        }
        if (outdated) {
            shrinkColumn(installedIndex, remaining);
            shrinkColumn(latestIndex, remaining);
        } else {
            shrinkColumn(versionIndex, remaining);
        }
        summaryWidth = availableWidth > usedNonSummaryWidth() ? availableWidth - usedNonSummaryWidth() : 0;
    }
    widths[summaryIndex] = std::max(summaryWidth, minSummaryHeaderWidth);

    out() << '\n';
    for (size_t i = 0; i < tableHeaders.size(); ++i) {
        if (i + 1 == tableHeaders.size()) {
            out() << decorateHeader(tableHeaders[i]);
        } else {
            out() << decorateHeader(plain_display_internal::pad_right(tableHeaders[i], widths[i]));
        }
        if (i + 1 < tableHeaders.size()) {
            out() << "  ";
        }
    }
    out() << '\n';
    for (size_t i = 0; i < tableHeaders.size(); ++i) {
        out() << decorateRule(plain_display_internal::repeat_char('-', static_cast<int>(widths[i])));
        if (i + 1 < tableHeaders.size()) {
            out() << "  ";
        }
    }
    out() << '\n';
    for (const auto& row : tableRows) {
        std::vector<std::vector<std::string>> wrappedCells;
        wrappedCells.reserve(tableHeaders.size());
        size_t rowHeight = 1;
        for (size_t i = 0; i < tableHeaders.size(); ++i) {
            const std::string cell = i < row.size() ? row[i] : std::string{};
            if (i == nameIndex || i == summaryIndex) {
                wrappedCells.push_back(wrapText(cell, widths[i]));
            } else {
                wrappedCells.push_back({plain_display_internal::truncate_middle(cell, widths[i])});
            }
            rowHeight = std::max(rowHeight, wrappedCells.back().size());
        }

        for (size_t lineIndex = 0; lineIndex < rowHeight; ++lineIndex) {
            for (size_t i = 0; i < tableHeaders.size(); ++i) {
                const std::string segment = lineIndex < wrappedCells[i].size() ? wrappedCells[i][lineIndex] : std::string{};
                if (i + 1 == tableHeaders.size()) {
                    out() << segment;
                } else {
                    out() << std::left << std::setw(static_cast<int>(widths[i])) << segment;
                }
                if (i + 1 < tableHeaders.size()) {
                    out() << "  ";
                }
            }
            out() << '\n';
        }
    }
    out() << '\n';
    out().flush();
}

void PlainDisplay::onTableBegin(const std::vector<std::string>& headers) {
    std::lock_guard<std::mutex> lock(mtx);
    tableHeaders = headers;
    tableRows.clear();
    colWidths.clear();
    fieldValueTable = currentMode == DisplayMode::INFO && headers.size() == 2 && headers[0] == "Field" && headers[1] == "Value";
    colWidths.reserve(headers.size());
    for (const auto& header : headers) {
        colWidths.push_back(std::max(header.size(), static_cast<size_t>(12)));
    }
}

void PlainDisplay::onTableRow(const std::vector<std::string>& cells) {
    std::lock_guard<std::mutex> lock(mtx);
    tableRows.push_back(cells);
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i >= colWidths.size()) {
            colWidths.resize(i + 1, 12);
        }
        colWidths[i] = std::max(colWidths[i], cells[i].size());
    }
}

void PlainDisplay::onTableEnd() {
    std::lock_guard<std::mutex> lock(mtx);
    if (fieldValueTable) {
        renderFieldValueTable();
        tableHeaders.clear();
        tableRows.clear();
        colWidths.clear();
        fieldValueTable = false;
        return;
    }
    if ((currentMode == DisplayMode::SEARCH || currentMode == DisplayMode::LIST || currentMode == DisplayMode::OUTDATED)
        && isPackageSummaryTable()) {
        renderWrappedPackageTable();
        tableHeaders.clear();
        tableRows.clear();
        colWidths.clear();
        fieldValueTable = false;
        return;
    }
    out() << '\n';
    for (size_t i = 0; i < tableHeaders.size(); ++i) {
        out() << decorateHeader(plain_display_internal::pad_right(tableHeaders[i], colWidths[i]));
        if (i + 1 < tableHeaders.size()) {
            out() << "  ";
        }
    }
    out() << '\n';
    for (size_t i = 0; i < tableHeaders.size(); ++i) {
        out() << decorateRule(plain_display_internal::repeat_char('-', static_cast<int>(colWidths[i])));
        if (i + 1 < tableHeaders.size()) {
            out() << "  ";
        }
    }
    out() << '\n';
    for (const auto& row : tableRows) {
        for (size_t i = 0; i < row.size(); ++i) {
            const size_t width = i < colWidths.size() ? colWidths[i] : 12;
            out() << std::left << std::setw(static_cast<int>(width)) << row[i];
            if (i + 1 < row.size()) {
                out() << "  ";
            }
        }
        out() << '\n';
    }
    out() << '\n';
    out().flush();
    tableHeaders.clear();
    tableRows.clear();
    colWidths.clear();
    fieldValueTable = false;
}
