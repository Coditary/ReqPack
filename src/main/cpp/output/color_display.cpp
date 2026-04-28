#include "output/color_display.h"
#include "output/ansi_color.h"

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Every hook delegates to PlainDisplay for the raw text, then wraps it.
// This way formatting (bar width, label alignment, etc.) never needs to be
// duplicated here.
// ─────────────────────────────────────────────────────────────────────────────

std::string ColorDisplay::decorateRule(const std::string& rule) const {
	return ansi_wrap(PlainDisplay::decorateRule(rule), scheme_.rule);
}

std::string ColorDisplay::decorateHeader(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateHeader(text), scheme_.header);
}

std::string ColorDisplay::decorateSummaryOk(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateSummaryOk(text), scheme_.summaryOk);
}

std::string ColorDisplay::decorateSummaryFail(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateSummaryFail(text), scheme_.summaryFail);
}

std::string ColorDisplay::decorateBarFill(int count) const {
	// Get the raw fill string from the base, then colorize it as a unit.
	return ansi_wrap(PlainDisplay::decorateBarFill(count), scheme_.barFill);
}

std::string ColorDisplay::decorateBarEmpty(int count) const {
	return ansi_wrap(PlainDisplay::decorateBarEmpty(count), scheme_.barEmpty);
}

std::string ColorDisplay::decorateBarOuter(const std::string& inner) const {
	// inner is already decorated; wrap only the brackets.
	const std::string withBrackets = PlainDisplay::decorateBarOuter(inner);
	if (scheme_.barOuter.empty() || scheme_.barOuter == "none") {
		return withBrackets;
	}
	// Only colorize the brackets, not the already-colored inner.
	const std::string open  = ansi_open(scheme_.barOuter);
	const std::string reset = "\033[0m";
	return open + "[" + reset + inner + open + "]" + reset;
}

std::string ColorDisplay::decorateStep(const std::string& step) const {
	return ansi_wrap(PlainDisplay::decorateStep(step), scheme_.step);
}

std::string ColorDisplay::decorateSuccessMarker(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateSuccessMarker(text), scheme_.successMarker);
}

std::string ColorDisplay::decorateFailureMarker(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateFailureMarker(text), scheme_.failureMarker);
}

std::string ColorDisplay::decorateMessage(const std::string& text) const {
	return ansi_wrap(PlainDisplay::decorateMessage(text), scheme_.message);
}
