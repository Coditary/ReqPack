#include "output/plain_display.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

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

} // namespace

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
		case DisplayMode::INFO:    return "INFO";
		case DisplayMode::ENSURE:  return "ENSURE";
		case DisplayMode::SBOM:    return "SBOM";
		default:                   return "REQPACK";
	}
}

void PlainDisplay::printRule() const {
	std::cout << decorateRule(repeatChar('-', RULE_WIDTH)) << '\n';
}

std::string PlainDisplay::formatItemLine(const std::string& itemId,
                                          int                percent,
                                          const std::string& step) const {
	std::ostringstream ss;
	ss << "  "
	   << std::left << std::setw(LABEL_WIDTH) << itemId
	   << "  " << renderBar(percent)
	   << "  " << std::setw(3) << percent << '%';
	if (!step.empty()) {
		ss << "  " << decorateStep(step);
	}
	return ss.str();
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
	std::cout << "  " << decorateHeader(header) << '\n';
	printRule();
	std::cout.flush();
}

void PlainDisplay::onSessionEnd(bool success, int succeeded, int failed) {
	std::lock_guard<std::mutex> lock(mtx);

	printRule();
	const std::string label = modeLabel(currentMode);
	if (succeeded > 0 || failed > 0) {
		const std::string summary = label + " done:  " +
		                            std::to_string(succeeded) + " ok,  " +
		                            std::to_string(failed) + " failed";
		if (failed > 0) {
			std::cout << "  " << decorateSummaryFail(summary) << '\n';
		} else {
			std::cout << "  " << decorateSummaryOk(summary) << '\n';
		}
	} else {
		const std::string summary = label + (success ? " done" : " failed");
		if (success) {
			std::cout << "  " << decorateSummaryOk(summary) << '\n';
		} else {
			std::cout << "  " << decorateSummaryFail(summary) << '\n';
		}
	}
	printRule();
	std::cout.flush();
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
			.progress = 0,
			.step     = "starting",
			.state    = DisplayItemState::RUNNING
		};
	}
	std::cout << formatItemLine(itemId, 0, "starting") << '\n';
	std::cout.flush();
}

void PlainDisplay::onItemProgress(const std::string& itemId, int percent) {
	std::string step;
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			it->second.progress = percent;
			step = it->second.step;
		}
	}
	std::cout << formatItemLine(itemId, percent, step) << '\n';
	std::cout.flush();
}

void PlainDisplay::onItemStep(const std::string& itemId,
                               const std::string& step) {
	int progress = 0;
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			it->second.step  = step;
			it->second.state = DisplayItemState::RUNNING;
			progress         = it->second.progress;
		}
	}
	std::cout << formatItemLine(itemId, progress, step) << '\n';
	std::cout.flush();
}

void PlainDisplay::onItemSuccess(const std::string& itemId) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			it->second.progress = 100;
			it->second.step     = "done";
			it->second.state    = DisplayItemState::SUCCESS;
		}
	}
	std::cout << formatItemLine(itemId, 100, "done") << '\n';
	std::cout << "  " << std::left << std::setw(LABEL_WIDTH) << itemId
	          << "  " << decorateSuccessMarker("OK") << '\n';
	std::cout.flush();
}

void PlainDisplay::onItemFailure(const std::string& itemId,
                                  const std::string& reason) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = itemMap.find(itemId);
		if (it != itemMap.end()) {
			it->second.state = DisplayItemState::FAILED;
		}
	}
	std::cout << "  " << std::left << std::setw(LABEL_WIDTH) << itemId
	          << "  " << decorateFailureMarker("[FAILED]");
	if (!reason.empty()) {
		std::cout << "  " << reason;
	}
	std::cout << '\n';
	std::cout.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic message
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onMessage(const std::string& text,
                              const std::string& source) {
	if (!source.empty()) {
		std::cout << "  [" << source << "]  " << decorateMessage(text) << '\n';
	} else {
		std::cout << "  " << decorateMessage(text) << '\n';
	}
	std::cout.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Table output
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::onTableBegin(const std::vector<std::string>& headers) {
	std::lock_guard<std::mutex> lock(mtx);
	tableHeaders = headers;
	colWidths.clear();
	colWidths.reserve(headers.size());
	for (const auto& h : headers) {
		colWidths.push_back(std::max(h.size(), static_cast<size_t>(12)));
	}

	std::cout << '\n';
	for (size_t i = 0; i < headers.size(); ++i) {
		std::cout << std::left << std::setw(static_cast<int>(colWidths[i]))
		          << decorateHeader(headers[i]);
		if (i + 1 < headers.size()) std::cout << "  ";
	}
	std::cout << '\n';

	// Underline.
	for (size_t i = 0; i < headers.size(); ++i) {
		std::cout << decorateRule(repeatChar('-', static_cast<int>(colWidths[i])));
		if (i + 1 < headers.size()) std::cout << "  ";
	}
	std::cout << '\n';
	std::cout.flush();
}

void PlainDisplay::onTableRow(const std::vector<std::string>& cells) {
	std::lock_guard<std::mutex> lock(mtx);
	for (size_t i = 0; i < cells.size(); ++i) {
		const size_t w    = (i < colWidths.size()) ? colWidths[i] : 12;
		const std::string& cell = cells[i];
		if (cell.size() > w) {
			std::cout << std::left << std::setw(static_cast<int>(w))
			          << (cell.substr(0, w - 1) + "~");
		} else {
			std::cout << std::left << std::setw(static_cast<int>(w)) << cell;
		}
		if (i + 1 < cells.size()) std::cout << "  ";
	}
	std::cout << '\n';
	std::cout.flush();
}

void PlainDisplay::onTableEnd() {
	std::cout << '\n';
	std::cout.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// Housekeeping
// ─────────────────────────────────────────────────────────────────────────────

void PlainDisplay::flush() {
	std::cout.flush();
}
