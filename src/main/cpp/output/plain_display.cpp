#include "output/plain_display.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>

#include <sys/ioctl.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string joinItems(const std::vector<std::string>& v) {
	std::string out;
	for (const auto& s : v) {
		if (!out.empty()) out += "  ";
		out += s;
	}
	return out;
}

std::string repeatChar(char c, int n) {
	return (n > 0) ? std::string(static_cast<size_t>(n), c) : std::string{};
}

void merge_progress_metrics(DisplayProgressMetrics& target, const DisplayProgressMetrics& update) {
	if (update.percent.has_value()) {
		target.percent = update.percent;
	}
	if (update.currentBytes.has_value()) {
		target.currentBytes = update.currentBytes;
	}
	if (update.totalBytes.has_value()) {
		target.totalBytes = update.totalBytes;
	}
	if (update.bytesPerSecond.has_value()) {
		target.bytesPerSecond = update.bytesPerSecond;
	}
}

size_t terminal_width() {
	if (const char* columns = std::getenv("COLUMNS")) {
		try {
			const size_t parsed = static_cast<size_t>(std::stoul(columns));
			if (parsed > 0) {
				return parsed;
			}
		} catch (...) {
		}
	}

	winsize size{};
	if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
		return size.ws_col;
	}

	return 100;
}

} // namespace

std::ostream& PlainDisplay::out() const {
	return std::cout;
}

// ─────────────────────────────────────────────────────────────────────────────
// Default decoration hooks — plain text, no ANSI
// ─────────────────────────────────────────────────────────────────────────────

std::string PlainDisplay::decorateRule(const std::string& rule) const {
	return rule;
}
std::string PlainDisplay::decorateHeader(const std::string& text) const {
	return text;
}
std::string PlainDisplay::decorateSummaryOk(const std::string& text) const {
	return text;
}
std::string PlainDisplay::decorateSummaryFail(const std::string& text) const {
	return text;
}
std::string PlainDisplay::decorateBarFill(int count) const {
	return repeatChar('#', count);
}
std::string PlainDisplay::decorateBarEmpty(int count) const {
	return repeatChar('.', count);
}
std::string PlainDisplay::decorateBarOuter(const std::string& inner) const {
	return "[" + inner + "]";
}
std::string PlainDisplay::decorateStep(const std::string& step) const {
	return step;
}
std::string PlainDisplay::decorateSuccessMarker(const std::string& text) const {
	return text;
}
std::string PlainDisplay::decorateFailureMarker(const std::string& text) const {
	return text;
}
std::string PlainDisplay::decorateMessage(const std::string& text) const {
	return text;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string PlainDisplay::renderBar(int percent) const {
	percent       = std::clamp(percent, 0, 100);
	const int filled = (percent * BAR_WIDTH) / 100;
	const int empty  = BAR_WIDTH - filled;
	const std::string inner = decorateBarFill(filled) + decorateBarEmpty(empty);
	return decorateBarOuter(inner);
}

/*static*/ std::string PlainDisplay::modeLabel(DisplayMode mode) {
	switch (mode) {
		case DisplayMode::INSTALL: return "INSTALL";
		case DisplayMode::REMOVE:  return "REMOVE";
		case DisplayMode::UPDATE:  return "UPDATE";
		case DisplayMode::SEARCH:  return "SEARCH";
		case DisplayMode::LIST:    return "LIST";
		case DisplayMode::OUTDATED:return "OUTDATED";
		case DisplayMode::INFO:    return "INFO";
		case DisplayMode::SNAPSHOT:return "SNAPSHOT";
		case DisplayMode::SERVE:   return "SERVE";
		case DisplayMode::REMOTE:  return "REMOTE";
		case DisplayMode::ENSURE:  return "ENSURE";
		case DisplayMode::SBOM:    return "SBOM";
		default:                   return "REQPACK";
	}
}

void PlainDisplay::printRule() const {
	out() << decorateRule(repeatChar('-', RULE_WIDTH)) << '\n';
}

std::string PlainDisplay::formatItemLine(const std::string& itemId,
	                                          const DisplayProgressMetrics& metrics,
	                                          const std::string& step) const {
	const DisplayProgressMetrics resolvedMetrics = canonicalize_progress_metrics(metrics);
	const int percent = resolvedMetrics.percent.value_or(0);
	std::ostringstream ss;
	ss << "  "
	   << std::left << std::setw(LABEL_WIDTH) << itemId
	   << "  " << renderBar(percent);
	const std::string summary = format_progress_summary(resolvedMetrics);
	if (!summary.empty()) {
		ss << "  " << decorateMessage(summary);
	}
	if (!step.empty()) {
		ss << "  " << decorateStep(step);
	}
	return ss.str();
}

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
					for (size_t offset = 0; offset < word.size(); offset += width) {
						lines.push_back(word.substr(offset, width));
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
				for (size_t offset = 0; offset < word.size(); offset += width) {
					const std::string segment = word.substr(offset, width);
					if (offset + width < word.size()) {
						lines.push_back(segment);
					} else {
						current = segment;
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
	return (tableHeaders.size() == 3
	        && tableHeaders[0] == "Name"
	        && tableHeaders[1] == "Version"
	        && tableHeaders[2] == "Summary")
	    || (tableHeaders.size() == 5
	        && tableHeaders[0] == "Name"
	        && tableHeaders[1] == "Version"
	        && tableHeaders[2] == "Type"
	        && tableHeaders[3] == "Architecture"
	        && tableHeaders[4] == "Description")
	    || (tableHeaders.size() == 4
	        && tableHeaders[0] == "System"
	        && tableHeaders[1] == "Name"
	        && tableHeaders[2] == "Version"
	        && tableHeaders[3] == "Summary")
	    || (tableHeaders.size() == 6
	        && tableHeaders[0] == "System"
	        && tableHeaders[1] == "Name"
	        && tableHeaders[2] == "Version"
	        && tableHeaders[3] == "Type"
	        && tableHeaders[4] == "Architecture"
	        && tableHeaders[5] == "Description");
}

void PlainDisplay::renderFieldValueTable() const {
	const size_t terminalWidth = terminal_width();
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
	const size_t terminalWidth = terminal_width();
	const bool hasSystem = tableHeaders.size() == 4 || tableHeaders.size() == 6;
	const bool extended = tableHeaders.size() == 5 || tableHeaders.size() == 6;
	const size_t systemIndex = hasSystem ? 0 : static_cast<size_t>(-1);
	const size_t nameIndex = hasSystem ? 1 : 0;
	const size_t versionIndex = hasSystem ? 2 : 1;
	const size_t typeIndex = extended ? (hasSystem ? 3 : 2) : static_cast<size_t>(-1);
	const size_t archIndex = extended ? (hasSystem ? 4 : 3) : static_cast<size_t>(-1);
	const size_t summaryIndex = extended ? (hasSystem ? 5 : 4) : (hasSystem ? 3 : 2);

	std::vector<size_t> widths(tableHeaders.size(), 0);
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		widths[i] = tableHeaders[i].size();
	}
	for (const auto& row : tableRows) {
		for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
			widths[i] = std::max(widths[i], row[i].size());
		}
	}

	if (hasSystem) {
		widths[systemIndex] = std::clamp(widths[systemIndex], static_cast<size_t>(6), static_cast<size_t>(12));
	}
	widths[nameIndex] = std::clamp(widths[nameIndex], static_cast<size_t>(18), static_cast<size_t>(36));
	widths[versionIndex] = std::clamp(widths[versionIndex], static_cast<size_t>(8), static_cast<size_t>(18));
	if (extended) {
		widths[typeIndex] = std::clamp(widths[typeIndex], static_cast<size_t>(6), static_cast<size_t>(12));
		widths[archIndex] = std::clamp(widths[archIndex], static_cast<size_t>(8), static_cast<size_t>(12));
	}

	const size_t separatorWidth = tableHeaders.size() > 1 ? (tableHeaders.size() - 1) * 2 : 0;
	size_t nonSummaryWidth = separatorWidth;
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		if (i != summaryIndex) {
			nonSummaryWidth += widths[i];
		}
	}

	size_t summaryWidth = terminalWidth > nonSummaryWidth ? terminalWidth - nonSummaryWidth : static_cast<size_t>(16);
	const size_t minSummaryWidth = 20;
	const size_t minNameWidth = 16;
	const size_t minVersionWidth = 8;
	const size_t minSystemWidth = 6;
	const size_t minTypeWidth = 6;
	const size_t minArchWidth = 8;
	if (summaryWidth < minSummaryWidth) {
		auto reclaim = [&](size_t index, size_t minWidth) {
			if (summaryWidth >= minSummaryWidth) {
				return;
			}
			const size_t reducible = widths[index] > minWidth ? widths[index] - minWidth : 0;
			const size_t needed = minSummaryWidth - summaryWidth;
			const size_t taken = std::min(reducible, needed);
			widths[index] -= taken;
			summaryWidth += taken;
		};
		reclaim(nameIndex, minNameWidth);
		reclaim(versionIndex, minVersionWidth);
		if (extended) {
			reclaim(typeIndex, minTypeWidth);
			reclaim(archIndex, minArchWidth);
		}
		if (hasSystem) {
			reclaim(systemIndex, minSystemWidth);
		}
	}
	widths[summaryIndex] = std::max(summaryWidth, tableHeaders[summaryIndex].size());

	out() << '\n';
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		out() << std::left << std::setw(static_cast<int>(widths[i])) << decorateHeader(tableHeaders[i]);
		if (i + 1 < tableHeaders.size()) {
			out() << "  ";
		}
	}
	out() << '\n';
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		out() << decorateRule(repeatChar('-', static_cast<int>(widths[i])));
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
			wrappedCells.push_back(wrapText(cell, widths[i]));
			rowHeight = std::max(rowHeight, wrappedCells.back().size());
		}

		for (size_t lineIndex = 0; lineIndex < rowHeight; ++lineIndex) {
			for (size_t i = 0; i < tableHeaders.size(); ++i) {
				const std::string segment = lineIndex < wrappedCells[i].size() ? wrappedCells[i][lineIndex] : std::string{};
				out() << std::left << std::setw(static_cast<int>(widths[i])) << segment;
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

// ─────────────────────────────────────────────────────────────────────────────
// Session
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onSessionBegin(DisplayMode                    mode,
                                   const std::vector<std::string>& items) {
	std::lock_guard<std::mutex> lock(mtx);
	currentMode = mode;
	itemMap.clear();
	for (const auto& id : items) {
		itemMap[id] = DisplayItemStatus{.id = id, .label = id};
	}

	std::string header = modeLabel(mode);
	if (!items.empty()) {
		header += ": " + joinItems(items);
	}

	printRule();
	out() << "  " << decorateHeader(header) << '\n';
	printRule();
	out().flush();
}

void PlainDisplay::onSessionEnd(bool success, int succeeded, int skipped, int failed) {
	std::lock_guard<std::mutex> lock(mtx);

	printRule();
	const std::string label = modeLabel(currentMode);
	if (succeeded > 0 || skipped > 0 || failed > 0) {
		const std::string summary = label + " done:  " +
		                            std::to_string(succeeded) + " ok,  " +
		                            std::to_string(skipped) + " skipped,  " +
		                            std::to_string(failed) + " failed";
		if (failed > 0) {
			out() << "  " << decorateSummaryFail(summary) << '\n';
		} else {
			out() << "  " << decorateSummaryOk(summary) << '\n';
		}
	} else {
		const std::string summary = label + (success ? " done" : " failed");
		if (success) {
			out() << "  " << decorateSummaryOk(summary) << '\n';
		} else {
			out() << "  " << decorateSummaryFail(summary) << '\n';
		}
	}
	printRule();
	out().flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-item
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onItemBegin(const std::string& itemId,
                                const std::string& label) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		itemMap[itemId] = DisplayItemStatus{
			.id       = itemId,
			.label    = label,
			.metrics  = DisplayProgressMetrics{.percent = 0},
			.step     = "starting",
			.state    = DisplayItemState::RUNNING
		};
	}
	out() << formatItemLine(itemId, DisplayProgressMetrics{.percent = 0}, "starting") << '\n';
	out().flush();
}


void PlainDisplay::onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) {
	DisplayProgressMetrics displayMetrics;
	std::string step;
	{
		std::lock_guard<std::mutex> lock(mtx);
		DisplayItemStatus& status = itemMap[itemId];
		if (status.id.empty()) {
			status.id = itemId;
			status.label = itemId;
		}
		merge_progress_metrics(status.metrics, metrics);
		status.metrics = canonicalize_progress_metrics(status.metrics, resolve_progress_percent(status.metrics));
		status.state = DisplayItemState::RUNNING;
		step = status.step;
		displayMetrics = status.metrics;
	}
	out() << formatItemLine(itemId, displayMetrics, step) << '\n';
	out().flush();
}

void PlainDisplay::onItemStep(const std::string& itemId,
	                               const std::string& step) {
	DisplayProgressMetrics metrics;
	{
		std::lock_guard<std::mutex> lock(mtx);
		DisplayItemStatus& status = itemMap[itemId];
		if (status.id.empty()) {
			status.id = itemId;
			status.label = itemId;
		}
		status.step  = step;
		status.state = DisplayItemState::RUNNING;
		metrics = status.metrics;
	}
	out() << formatItemLine(itemId, metrics, step) << '\n';
	out().flush();
}

void PlainDisplay::onItemSuccess(const std::string& itemId) {
	bool alreadySucceeded = false;
	DisplayProgressMetrics metrics{.percent = 100};
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			alreadySucceeded = it->second.state == DisplayItemState::SUCCESS;
			it->second.metrics.percent = 100;
			if (it->second.metrics.totalBytes.has_value()) {
				it->second.metrics.currentBytes = it->second.metrics.totalBytes;
			}
			it->second.metrics = canonicalize_progress_metrics(it->second.metrics, 100);
			it->second.step    = "done";
			it->second.state   = DisplayItemState::SUCCESS;
			metrics = it->second.metrics;
		}
	}
	if (alreadySucceeded) {
		return;
	}
	out() << formatItemLine(itemId, metrics, "done") << '\n';
	out() << "  " << std::left << std::setw(LABEL_WIDTH) << itemId
	      << "  " << decorateSuccessMarker("OK") << '\n';
	out().flush();
}

void PlainDisplay::onItemFailure(const std::string& itemId,
                                   const std::string& reason) {
	bool alreadyFailed = false;
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			alreadyFailed = it->second.state == DisplayItemState::FAILED;
			it->second.state = DisplayItemState::FAILED;
		}
	}
	if (alreadyFailed) {
		return;
	}
	out() << "  " << std::left << std::setw(LABEL_WIDTH) << itemId
	      << "  " << decorateFailureMarker("[FAILED]");
	if (!reason.empty()) {
		out() << "  " << reason;
	}
	out() << '\n';
	out().flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic message
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onMessage(const std::string& text,
                              const std::string& source) {
	if (!source.empty()) {
		out() << "  [" << source << "]  " << decorateMessage(text) << '\n';
	} else {
		out() << "  " << decorateMessage(text) << '\n';
	}
	out().flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Table output
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onTableBegin(const std::vector<std::string>& headers) {
	std::lock_guard<std::mutex> lock(mtx);
	tableHeaders = headers;
	tableRows.clear();
	colWidths.clear();
	fieldValueTable = currentMode == DisplayMode::INFO
	    && headers.size() == 2
	    && headers[0] == "Field"
	    && headers[1] == "Value";
	colWidths.reserve(headers.size());
	for (const auto& h : headers) {
		colWidths.push_back(std::max(h.size(), static_cast<size_t>(12)));
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
	const size_t tableWidth = std::accumulate(colWidths.begin(), colWidths.end(), static_cast<size_t>(0))
	    + (tableHeaders.size() > 1 ? (tableHeaders.size() - 1) * 2 : 0);
	if (currentMode == DisplayMode::SEARCH && isPackageSummaryTable() && tableWidth > terminal_width()) {
		renderWrappedPackageTable();
		tableHeaders.clear();
		tableRows.clear();
		colWidths.clear();
		fieldValueTable = false;
		return;
	}
	out() << '\n';
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		out() << std::left << std::setw(static_cast<int>(colWidths[i]))
		      << decorateHeader(tableHeaders[i]);
		if (i + 1 < tableHeaders.size()) out() << "  ";
	}
	out() << '\n';
	for (size_t i = 0; i < tableHeaders.size(); ++i) {
		out() << decorateRule(repeatChar('-', static_cast<int>(colWidths[i])));
		if (i + 1 < tableHeaders.size()) out() << "  ";
	}
	out() << '\n';
	for (const auto& row : tableRows) {
		for (size_t i = 0; i < row.size(); ++i) {
			const size_t w = (i < colWidths.size()) ? colWidths[i] : 12;
			out() << std::left << std::setw(static_cast<int>(w)) << row[i];
			if (i + 1 < row.size()) out() << "  ";
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

// ─────────────────────────────────────────────────────────────────────────────
// Housekeeping
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::flush() {
	out().flush();
}
