#pragma once

#include "core/config/configuration.h"
#include "output/live_display.h"

#include <string>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// ColorDisplay — LiveDisplay with ANSI color applied to each semantic role.
//
// Inherits all formatting logic from LiveDisplay / PlainDisplay.  Overrides every decorate*
// hook to wrap the text with the corresponding ANSI spec from the scheme.
//
// DisplayColorScheme is defined in core/config/configuration.h so it can be loaded
// from the Lua config and passed straight through to this class.
// ─────────────────────────────────────────────────────────────────────────────
class ColorDisplay : public LiveDisplay {
public:
	explicit ColorDisplay(DisplayColorScheme scheme = {}) noexcept
	    : scheme_(std::move(scheme)) {}

protected:
	std::string decorateRule(const std::string& rule) const override;
	std::string decorateHeader(const std::string& text) const override;
	std::string decorateSummaryOk(const std::string& text) const override;
	std::string decorateSummaryFail(const std::string& text) const override;
	std::string decorateBarFill(int count) const override;
	std::string decorateBarEmpty(int count) const override;
	std::string decorateBarOuter(const std::string& inner) const override;
	std::string decorateStep(const std::string& step) const override;
	std::string decorateSuccessMarker(const std::string& text) const override;
	std::string decorateFailureMarker(const std::string& text) const override;
	std::string decorateSkippedMarker(const std::string& text) const override;
	std::string decorateMessage(const std::string& text) const override;
	std::string decorateListNameCell(const std::string& text) const override;
	std::string decorateListVersionCell(const std::string& text) const override;
	std::string decorateListTypeCell(const std::string& text) const override;
	std::string decorateListPlatformCell(const std::string& text) const override;
	std::string decorateListDescriptionCell(const std::string& text) const override;
	std::string applySearchHighlight(const std::string& text) const override;
	std::string decorateSpinnerFrame(std::string_view frame) const override;
	std::string format_session_summary(bool success, int succeeded, int skipped, int failed) const override;

private:
	DisplayColorScheme scheme_;
};
