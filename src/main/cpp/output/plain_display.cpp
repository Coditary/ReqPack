#include "output/plain_display.h"

#include "plain_display_internal.h"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <sys/ioctl.h>
#include <unistd.h>

namespace plain_display_internal {

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

std::string repeat_char(char value, int count) {
    return count > 0 ? std::string(static_cast<size_t>(count), value) : std::string{};
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

std::string PlainDisplay::decorateSummaryOk(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateSummaryFail(const std::string& text) const {
    return text;
}

std::string PlainDisplay::decorateBarFill(int count) const {
    return plain_display_internal::repeat_char('#', count);
}

std::string PlainDisplay::decorateBarEmpty(int count) const {
    return plain_display_internal::repeat_char('.', count);
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

std::string PlainDisplay::renderBar(int percent) const {
    percent = std::clamp(percent, 0, 100);
    const int filled = (percent * BAR_WIDTH) / 100;
    const int empty = BAR_WIDTH - filled;
    const std::string inner = decorateBarFill(filled) + decorateBarEmpty(empty);
    return decorateBarOuter(inner);
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
