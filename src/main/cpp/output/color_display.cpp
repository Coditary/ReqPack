#include "output/color_display.h"
#include "output/ansi_color.h"

#include <cctype>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Every hook delegates to LiveDisplay / PlainDisplay for the raw text, then wraps it.
// This way formatting (bar width, label alignment, etc.) never needs to be
// duplicated here.
// ─────────────────────────────────────────────────────────────────────────────

std::string ColorDisplay::decorateRule(const std::string& rule) const {
	return ansi_wrap(LiveDisplay::decorateRule(rule), scheme_.rule);
}

std::string ColorDisplay::decorateHeader(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateHeader(text), scheme_.header);
}

std::string ColorDisplay::decorateSummaryOk(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateSummaryOk(text), scheme_.summaryOk);
}

std::string ColorDisplay::decorateSummaryFail(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateSummaryFail(text), scheme_.summaryFail);
}

std::string ColorDisplay::decorateBarFill(int count) const {
	// Get the raw fill string from the base, then colorize it as a unit.
	return ansi_wrap(LiveDisplay::decorateBarFill(count), scheme_.barFill);
}

std::string ColorDisplay::decorateBarEmpty(int count) const {
	return ansi_wrap(LiveDisplay::decorateBarEmpty(count), scheme_.barEmpty);
}

std::string ColorDisplay::decorateBarOuter(const std::string& inner) const {
	// inner is already decorated; wrap only the brackets.
	const std::string withBrackets = LiveDisplay::decorateBarOuter(inner);
	if (scheme_.barOuter.empty() || scheme_.barOuter == "none") {
		return withBrackets;
	}
	// Only colorize the brackets, not the already-colored inner.
	const std::string open  = ansi_open(scheme_.barOuter);
	const std::string reset = "\033[0m";
	return open + "[" + reset + inner + open + "]" + reset;
}

std::string ColorDisplay::decorateStep(const std::string& step) const {
	return ansi_wrap(LiveDisplay::decorateStep(step), scheme_.step);
}

std::string ColorDisplay::decorateSuccessMarker(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateSuccessMarker(text), scheme_.successMarker);
}

std::string ColorDisplay::decorateFailureMarker(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateFailureMarker(text), scheme_.failureMarker);
}

std::string ColorDisplay::decorateSkippedMarker(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateSkippedMarker(text), scheme_.step);
}

std::string ColorDisplay::decorateMessage(const std::string& text) const {
	return ansi_wrap(LiveDisplay::decorateMessage(text), scheme_.message);
}

std::string ColorDisplay::decorateSpinnerFrame(const std::string_view frame) const {
	return ansi_wrap(std::string(frame), scheme_.step.empty() ? "cyan" : scheme_.step);
}

std::string ColorDisplay::format_session_summary(const bool success,
                                                 const int succeeded,
                                                 const int skipped,
                                                 const int failed) const {
	const std::string plain = LiveDisplay::format_session_summary(success, succeeded, skipped, failed);
	if (plain.empty()) {
		return plain;
	}
	if (plain.rfind("Found ", 0) == 0) {
		return decorateSummaryOk(plain);
	}

	const std::string dim = "bright_black";
	const std::string okSpec = scheme_.summaryOk.empty() ? "bold green" : scheme_.summaryOk;
	const std::string failSpec = scheme_.summaryFail.empty() ? "bold red" : scheme_.summaryFail;

	std::ostringstream stream;
	if (plain.rfind("🗑️", 0) == 0) {
		stream << "🗑️ ";
		stream << ansi_wrap(std::to_string(succeeded) + " removed", okSpec);
	} else {
		stream << "✨ ";
		const char* verb = plain.find("installed") != std::string::npos ? "installed" : "updated";
		stream << ansi_wrap(std::to_string(succeeded) + " " + verb, okSpec);
	}
	stream << ansi_wrap(", " + std::to_string(skipped) + " skipped", skipped > 0 ? scheme_.step : dim);
	stream << ansi_wrap(", " + std::to_string(failed) + " failed", failed > 0 ? failSpec : dim);
	if (!success) {
		return decorateSummaryFail(stream.str());
	}
	return stream.str();
}

std::string ColorDisplay::applySearchHighlight(const std::string& text) const {
	if (packageTableContext.highlightTerms.empty() || text.empty()) {
		return text;
	}

	std::string lowerText;
	lowerText.reserve(text.size());
	for (const char character : text) {
		lowerText.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
	}

	std::string result;
	for (std::size_t index = 0; index < text.size();) {
		std::size_t matchIndex = text.size();
		std::size_t matchLength = 0;
		for (const std::string& term : packageTableContext.highlightTerms) {
			if (term.empty()) {
				continue;
			}
			const std::size_t found = lowerText.find(term, index);
			if (found != std::string::npos && found < matchIndex) {
				matchIndex = found;
				matchLength = term.size();
			}
		}
		if (matchIndex >= text.size()) {
			result.append(text, index);
			break;
		}
		result.append(text, index, matchIndex - index);
		result.append(ansi_wrap(text.substr(matchIndex, matchLength), "bold yellow"));
		index = matchIndex + matchLength;
	}
	return result;
}

std::string ColorDisplay::decorateListNameCell(const std::string& text) const {
	return ansi_wrap(applySearchHighlight(text), "bold cyan");
}

std::string ColorDisplay::decorateListVersionCell(const std::string& text) const {
	return ansi_wrap(text, "dim green");
}

std::string ColorDisplay::decorateListTypeCell(const std::string& text) const {
	if (text == "plugin" || text == "pkg-mgr") {
		return ansi_wrap(text, "magenta");
	}
	if (text == "alias") {
		return ansi_wrap(text, "dim white");
	}
	if (text == "pkg") {
		return ansi_wrap(text, "blue");
	}
	return ansi_wrap(text, "dim");
}

std::string ColorDisplay::decorateListPlatformCell(const std::string& text) const {
	return ansi_wrap(text, "dim");
}

std::string ColorDisplay::decorateListDescriptionCell(const std::string& text) const {
	return ansi_wrap(applySearchHighlight(text), "dim white");
}
