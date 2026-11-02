#include "output/plain_display.h"

#include "plain_display_internal.h"

#include <reqpack/terminal_width.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <wchar.h>

namespace plain_display_internal {

namespace {

bool decode_utf8_codepoint(const std::string& text, const size_t index, wchar_t& codepoint, size_t& bytes) {
	if (index >= text.size()) {
		return false;
	}
	const unsigned char lead = static_cast<unsigned char>(text[index]);
	if (lead < 0x80) {
		codepoint = static_cast<wchar_t>(lead);
		bytes = 1;
		return true;
	}
	if ((lead & 0xE0) == 0xC0 && index + 1 < text.size()) {
		codepoint = static_cast<wchar_t>(((lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F));
		bytes = 2;
		return true;
	}
	if ((lead & 0xF0) == 0xE0 && index + 2 < text.size()) {
		codepoint = static_cast<wchar_t>(((lead & 0x0F) << 12)
		                                 | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6)
		                                 | (static_cast<unsigned char>(text[index + 2]) & 0x3F));
		bytes = 3;
		return true;
	}
	if ((lead & 0xF8) == 0xF0 && index + 3 < text.size()) {
		codepoint = static_cast<wchar_t>(((lead & 0x07) << 18)
		                                 | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12)
		                                 | ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6)
		                                 | (static_cast<unsigned char>(text[index + 3]) & 0x3F));
		bytes = 4;
		return true;
	}
	codepoint = static_cast<wchar_t>(lead);
	bytes = 1;
	return true;
}

size_t codepoint_display_width(const wchar_t codepoint) {
	if (codepoint == 0) {
		return 0;
	}
	const auto cp = static_cast<uint32_t>(codepoint);
	// Emoji / symbol blocks that occupy two terminal cells in common emulators.
	if ((cp >= 0x1F000 && cp <= 0x1FFFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xFE00 && cp <= 0xFE0F)) {
		return 2;
	}
	const int width = wcwidth(codepoint);
	if (width > 0) {
		return static_cast<size_t>(width);
	}
	return 1;
}

bool skip_terminal_escape_sequence(const std::string& text, size_t& index) {
	if (index >= text.size()) {
		return false;
	}
	const unsigned char byte = static_cast<unsigned char>(text[index]);
	if (byte != '\033' && byte != '\x1b') {
		return false;
	}
	const size_t sequenceStart = index;
	++index;
	if (index < text.size() && text[index] == '[') {
		++index;
		while (index < text.size()) {
			const unsigned char code = static_cast<unsigned char>(text[index]);
			if (code >= 0x40 && code <= 0x7E) {
				++index;
				break;
			}
			++index;
		}
	} else if (index < text.size() && text[index] == ']') {
		++index;
		while (index < text.size()) {
			if (text[index] == '\033' || text[index] == '\x1b') {
				if (index + 1 < text.size() && text[index + 1] == '\\') {
					index += 2;
					break;
				}
			}
			if (text[index] == '\x07') {
				++index;
				break;
			}
			++index;
		}
	} else if (index == sequenceStart + 1) {
		++index;
	}
	return true;
}

size_t glyph_display_width_at(const std::string& text, size_t& index) {
	if (skip_terminal_escape_sequence(text, index)) {
		return 0;
	}
	wchar_t codepoint = 0;
	size_t bytes = 0;
	if (!decode_utf8_codepoint(text, index, codepoint, bytes)) {
		++index;
		return 1;
	}
	const size_t width = codepoint_display_width(codepoint);
	index += bytes;
	return width;
}

}  // namespace

std::string join_items(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) {
            out += "  ";
        }
        out += value;
    }
    return out;
}

std::string format_session_item_list(const std::vector<std::string>& values) {
    if (values.empty()) {
        return {};
    }
    if (values.size() == 1) {
        return values.front();
    }
    if (values.size() <= 3) {
        std::string out;
        for (size_t index = 0; index < values.size(); ++index) {
            if (index > 0) {
                out += ", ";
            }
            out += values[index];
        }
        return out;
    }
    return std::to_string(values.size()) + " packages";
}

std::optional<std::string> common_session_system(const std::vector<std::string>& itemIds) {
    if (itemIds.size() < 2) {
        return std::nullopt;
    }
    std::string sharedSystem;
    for (const std::string& itemId : itemIds) {
        const std::size_t separator = itemId.find(':');
        if (separator == std::string::npos || separator == 0) {
            return std::nullopt;
        }
        const std::string system = itemId.substr(0, separator);
        if (sharedSystem.empty()) {
            sharedSystem = system;
        } else if (sharedSystem != system) {
            return std::nullopt;
        }
    }
    return sharedSystem.empty() ? std::nullopt : std::optional<std::string>(sharedSystem);
}

std::string transaction_action_verb(const DisplayMode mode) {
    switch (mode) {
        case DisplayMode::INSTALL:
            return "Installing";
        case DisplayMode::REMOVE:
            return "Removing";
        case DisplayMode::UPDATE:
            return "Updating";
        default:
            return {};
    }
}

std::string action_banner_text(const DisplayMode mode, const std::vector<std::string>& items) {
    const std::string joined = format_session_item_list(items);
    std::ostringstream banner;
    switch (mode) {
        case DisplayMode::SEARCH:
            banner << (joined.empty() ? "Searching" : "Searching " + joined);
            break;
        case DisplayMode::INFO:
            banner << (joined.empty() ? "Showing package info" : "Info for " + joined);
            break;
        case DisplayMode::LIST:
            banner << (joined.empty() ? "Listing packages" : "Listing " + joined);
            break;
        case DisplayMode::INSTALL:
        case DisplayMode::REMOVE:
        case DisplayMode::UPDATE: {
            const std::string verb = transaction_action_verb(mode);
            if (items.size() >= 2) {
                if (const std::optional<std::string> system = common_session_system(items); system.has_value()) {
                    banner << verb << ' ' << items.size() << " packages (" << *system << ')';
                    break;
                }
            }
            if (joined.empty()) {
                banner << verb << " packages";
            } else {
                banner << verb << ' ' << joined;
            }
            break;
        }
        default:
            banner << "Running";
            if (!joined.empty()) {
                banner << ' ' << joined;
            }
            break;
    }
    banner << "...";
    return banner.str();
}

std::string format_item_action_line(const char* marker, const std::string& body) {
    return std::string(kIndent) + marker + (body.empty() ? "" : " " + body);
}

std::string item_outcome_phrase(const DisplayMode mode) {
	switch (mode) {
		case DisplayMode::INSTALL:
			return "installed";
		case DisplayMode::REMOVE:
			return "removed";
		case DisplayMode::UPDATE:
			return "updated";
		default:
			return "done";
	}
}

bool is_generic_plugin_failure_summary(const std::string& reason) {
	if (reason.empty()) {
		return false;
	}
	if (reason.rfind("Plugin action failed for ", 0) == 0) {
		return true;
	}
	if (reason.rfind("Plugin action failed", 0) == 0) {
		return true;
	}
	if (reason.rfind("Package is unavailable:", 0) == 0) {
		return false;
	}
	return false;
}

std::string format_item_failure_primary(const std::string& itemId, const std::string& reason) {
	if (reason.empty()) {
		return itemId + " failed";
	}
	return itemId + " failed: " + reason;
}

std::optional<std::string> failure_hint_for_reason(const std::string& itemId, const std::string& reason) {
	(void)itemId;
	if (reason.find("rqp repositories not configured") != std::string::npos) {
		return "Configure a repository in ~/.config/reqpack/config.lua or pass --registry <path>";
	}
	if (reason.find("not found in rqp repositories") != std::string::npos) {
		return "Check the package name and configured rqp repository indexes";
	}
	if (reason.rfind("Unable to locate package", 0) == 0 || reason.find("Unable to locate package") != std::string::npos) {
		return "Run your package manager update command or verify the package name";
	}
	return std::nullopt;
}

std::string format_item_success_body(const std::string& itemId, const DisplayMode mode, const std::string& version) {
	std::ostringstream body;
	body << itemId;
	if (!version.empty()) {
		body << " (" << version << ')';
	}
	body << ' ' << item_outcome_phrase(mode);
	return body.str();
}

void write_session_summary(std::ostream& stream, const DisplayMode mode, const int succeeded, const int skipped,
                           const int failed) {
	const char* verb = "updated";
	switch (mode) {
		case DisplayMode::INSTALL:
			verb = "installed";
			break;
		case DisplayMode::REMOVE:
			verb = "removed";
			break;
		default:
			break;
	}
	stream << "Summary: " << succeeded << ' ' << verb;
	if (skipped > 0) {
		stream << ", " << skipped << " skipped";
	}
	if (failed > 0) {
		stream << ", " << failed << " failed";
	}
}

bool progress_has_byte_transfer(const DisplayProgressMetrics& metrics) {
	return metrics.currentBytes.has_value() || metrics.totalBytes.has_value();
}

std::string repeat_char(char value, int count) {
    return count > 0 ? std::string(static_cast<size_t>(count), value) : std::string{};
}

std::string repeat_visual_unit(const std::string& unit, const size_t count) {
    std::string repeated;
    repeated.reserve(unit.size() * count);
    for (size_t index = 0; index < count; ++index) {
        repeated += unit;
    }
    return repeated;
}

std::string pad_right(const std::string& text, size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
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

std::string trim_trailing_newlines(const std::string_view value) {
	std::string trimmed(value);
	while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
		trimmed.pop_back();
	}
	return trimmed;
}

namespace {

std::string trim_message_copy(std::string value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

bool starts_with_prefix(const std::string_view text, const std::string_view prefix) {
	return text.rfind(prefix, 0) == 0;
}

} // namespace

bool is_transaction_chatter_message(const std::string& text) {
	const std::string trimmed = trim_message_copy(text);
	if (trimmed.empty()) {
		return true;
	}
	if (starts_with_prefix(trimmed, "already on release") || starts_with_prefix(trimmed, "Already up to date")
	    || starts_with_prefix(trimmed, "now on release")) {
		return true;
	}
	if (starts_with_prefix(trimmed, "link:") || starts_with_prefix(trimmed, "Linked to")) {
		return true;
	}
	if (starts_with_prefix(trimmed, "Note: Running self-update")) {
		return true;
	}
	if (starts_with_prefix(trimmed, "status ")) {
		return true;
	}
	if (trimmed == "done") {
		return true;
	}
	if (starts_with_prefix(trimmed, "progress")) {
		return true;
	}
	return false;
}

TranscriptLineDecoration transcript_line_decoration(const std::string_view trimmed) {
	if (starts_with_prefix(trimmed, "already on release") || starts_with_prefix(trimmed, "Already up to date")) {
		return TranscriptLineDecoration::Info;
	}
	if (starts_with_prefix(trimmed, "link:") || starts_with_prefix(trimmed, "Linked to")
	    || starts_with_prefix(trimmed, "now on release")) {
		return TranscriptLineDecoration::Link;
	}
	return TranscriptLineDecoration::None;
}

bool stdout_is_tty() {
    return isatty(STDOUT_FILENO);
}

size_t visual_length(const std::string& text) {
	size_t length = 0;
	for (size_t index = 0; index < text.size();) {
		length += glyph_display_width_at(text, index);
	}
	return length;
}

std::string pad_right_visual(const std::string& text, const size_t width) {
    if (visual_length(text) >= width) {
        return text;
    }
    return text + std::string(width - visual_length(text), ' ');
}

void write_padded_cell(std::ostream& stream, const std::string& segment, const size_t width) {
    stream << segment;
    const size_t padding = width > visual_length(segment) ? width - visual_length(segment) : 0;
    stream << std::string(padding, ' ');
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

    return reqpack_terminal_column_count(100);
}

std::string truncate_middle(const std::string& text, size_t width) {
    if (width == 0 || text.size() <= width) {
        return text;
    }
    if (width <= 3) {
        return text.substr(0, width);
    }
    const size_t prefixWidth = (width - 3) / 2;
    const size_t suffixWidth = width - 3 - prefixWidth;
    return text.substr(0, prefixWidth) + "..." + text.substr(text.size() - suffixWidth);
}

std::string truncate_end(const std::string& text, size_t width) {
    constexpr const char* kEllipsis = "\xe2\x80\xa6";
    if (width == 0 || visual_length(text) <= width) {
        return text;
    }
    if (width == 1) {
        return kEllipsis;
    }
	std::string result;
	size_t used = 0;
	const size_t ellipsisWidth = codepoint_display_width(L'\u2026');
	const size_t budget = width > ellipsisWidth ? width - ellipsisWidth : 0;
	for (size_t index = 0; index < text.size();) {
		const size_t before = index;
		const size_t glyphWidth = glyph_display_width_at(text, index);
		if (glyphWidth == 0) {
			continue;
		}
		if (used + glyphWidth > budget) {
			break;
		}
		result.append(text, before, index - before);
		used += glyphWidth;
	}
	return result + kEllipsis;
}

std::optional<std::string> session_summary_text(const DisplayMode mode,
                                                const bool success,
                                                const int succeeded,
                                                const int skipped,
                                                const int failed) {
    switch (mode) {
        case DisplayMode::INFO:
            return std::nullopt;
        case DisplayMode::SEARCH:
        case DisplayMode::LIST:
        case DisplayMode::OUTDATED:
            return std::nullopt;
        default:
            break;
    }

    if (succeeded > 0 || skipped > 0 || failed > 0) {
        std::ostringstream stream;
        write_session_summary(stream, mode, succeeded, skipped, failed);
        return stream.str();
    }
    if (mode == DisplayMode::SEARCH || mode == DisplayMode::LIST) {
        return std::nullopt;
    }
    return success ? std::optional<std::string>("done") : std::optional<std::string>("failed");
}

std::vector<std::string> split_long_token(const std::string& token, size_t width) {
    if (width == 0 || token.size() <= width) {
        return {token};
    }

    std::vector<std::string> segments;
    size_t offset = 0;
    while (offset < token.size()) {
        const size_t remaining = token.size() - offset;
        if (remaining <= width) {
            segments.push_back(token.substr(offset));
            break;
        }

        const std::string chunk = token.substr(offset, width);
        size_t splitAt = width;
        const size_t delimiter = chunk.find_last_of("-._:/+");
        if (delimiter != std::string::npos && delimiter + 1 >= width / 3 && delimiter + 1 < width) {
            splitAt = delimiter + 1;
        }

        segments.push_back(token.substr(offset, splitAt));
        offset += splitAt;
    }

    return segments;
}

}  // namespace plain_display_internal

std::ostream& PlainDisplay::out() const {
    return std::cout;
}

std::string PlainDisplay::decorateRule(const std::string& rule) const {
    return rule;
}

std::string PlainDisplay::decorateHeader(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateActionBanner(const std::string& banner) const {
    return banner;
}

std::string PlainDisplay::decorateSummaryOk(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateSummaryFail(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateBarFill(int count) const {
	return plain_display_internal::repeat_visual_unit(plain_display_internal::kBarDoneUnit,
	                                                  static_cast<size_t>(count));
}

std::string PlainDisplay::decorateBarEmpty(int count) const {
	return plain_display_internal::repeat_visual_unit(plain_display_internal::kBarTodoUnit,
	                                                  static_cast<size_t>(count));
}

std::string PlainDisplay::decorateBarOuter(const std::string& inner) const {
    return inner;
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

std::string PlainDisplay::decorateFailureHint(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateSuccessVersion(const std::string& version) const {
    return version;
}

std::string PlainDisplay::decorateSkippedMarker(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateMessage(const std::string& text) const {
    return text;
}

void PlainDisplay::setPackageTableContext(const PackageTableDisplayContext& context) {
    std::lock_guard<std::mutex> lock(mtx);
    packageTableContext = context;
}

std::string PlainDisplay::applySearchHighlight(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateListNameCell(const std::string& text) const {
    return applySearchHighlight(text);
}

std::string PlainDisplay::decorateListVersionCell(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateListTypeCell(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateListPlatformCell(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateListDescriptionCell(const std::string& text) const {
    return applySearchHighlight(text);
}

std::string PlainDisplay::renderBar(int percent) const {
    percent = std::clamp(percent, 0, 100);
    const int filled = (percent * BAR_WIDTH) / 100;
    const int empty = BAR_WIDTH - filled;
    return decorateBarFill(filled) + decorateBarEmpty(empty);
}

std::string PlainDisplay::modeLabel(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::INSTALL:
            return "INSTALL";
        case DisplayMode::REMOVE:
            return "REMOVE";
        case DisplayMode::UPDATE:
            return "UPDATE";
        case DisplayMode::SEARCH:
            return "SEARCH";
        case DisplayMode::LIST:
            return "LIST";
        case DisplayMode::OUTDATED:
            return "OUTDATED";
        case DisplayMode::INFO:
            return "INFO";
        case DisplayMode::SNAPSHOT:
            return "SNAPSHOT";
        case DisplayMode::PACK:
            return "PACK";
        case DisplayMode::SERVE:
            return "SERVE";
        case DisplayMode::REMOTE:
            return "REMOTE";
        case DisplayMode::ENSURE:
            return "ENSURE";
        case DisplayMode::SBOM:
            return "SBOM";
        default:
            return "REQPACK";
    }
}

void PlainDisplay::printRule() const {
    out() << decorateRule(plain_display_internal::repeat_char('-', RULE_WIDTH)) << '\n';
}

std::string PlainDisplay::formatItemLine(
    const std::string& itemId,
    const DisplayProgressMetrics& metrics,
    const std::string& step
) const {
    const DisplayProgressMetrics resolvedMetrics = canonicalize_progress_metrics(metrics);
    const int percent = resolvedMetrics.percent.value_or(0);
    std::ostringstream stream;
    stream << "  " << std::left << std::setw(LABEL_WIDTH) << itemId << "  " << renderBar(percent);
    const std::string summary = format_progress_summary(resolvedMetrics);
    if (!summary.empty()) {
        stream << "  " << decorateMessage(summary);
    }
    if (!step.empty()) {
        stream << "  " << decorateStep(step);
    }
    return stream.str();
}

void PlainDisplay::flush() {
    out().flush();
}
