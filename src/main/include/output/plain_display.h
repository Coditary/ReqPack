#pragma once

#include "output/idisplay.h"

#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PlainDisplay — first IDisplay implementation.
//
// Output style:
//   - ASCII separator lines ( ----… )
//   - Progress bars built from '#' (filled) and '.' (empty)
//   - Each event emits a new line; no cursor movement / ANSI codes used.
//   - Column-aligned tables for LIST / SEARCH results.
//
// Subclass hooks:
//   Override the protected decorate* methods to add color, emphasis, or any
//   other terminal decoration.  ColorDisplay does exactly this.
//
// Example:
//
//   --------------------------------------------------
//   INSTALL: curl  wget  htop
//   --------------------------------------------------
//     curl             [##########..........] 50%  Downloading
//     curl             [####################] 100% done
//     curl             OK
//     wget             [####................] 20%  Connecting
//     ...
//   --------------------------------------------------
//   INSTALL done: 2 ok, 0 failed
//   --------------------------------------------------
// ─────────────────────────────────────────────────────────────────────────────
class PlainDisplay : public IDisplay {
public:
	// Width of the '#' / '.' progress bar (characters between the brackets).
	static constexpr int BAR_WIDTH   = 20;
	// Total width of the separator line.
	static constexpr int RULE_WIDTH  = 50;
	// Column width used when printing the item id prefix.
	static constexpr int LABEL_WIDTH = 16;

	// ── IDisplay ─────────────────────────────────────────────────────────────

	void onSessionBegin(DisplayMode mode,
	                    const std::vector<std::string>& items) override;
	void onSessionEnd(bool success, int succeeded, int skipped, int failed) override;

	void onItemBegin(const std::string& itemId,
	                 const std::string& label) override;
	void onItemProgress(const std::string& itemId,
	                    const DisplayProgressMetrics& metrics) override;
	void onItemStep(const std::string& itemId,
	                const std::string& step) override;
	void onItemSuccess(const std::string& itemId) override;
	void onItemFailure(const std::string& itemId,
	                   const std::string& reason) override;

	void onMessage(const std::string& text,
	               const std::string& source = {}) override;

	void onTableBegin(const std::vector<std::string>& headers) override;
	void onTableRow(const std::vector<std::string>& cells) override;
	void onTableEnd() override;

	void flush() override;

protected:
	// ── Decoration hooks (override in subclasses for colour / emphasis) ───────

	virtual std::ostream& out() const;

	/// Separator rule string (the `---…` line).
	virtual std::string decorateRule(const std::string& rule) const;

	/// Session header text (e.g. "INSTALL: curl  wget").
	virtual std::string decorateHeader(const std::string& text) const;

	/// Session summary line when everything succeeded.
	virtual std::string decorateSummaryOk(const std::string& text) const;

	/// Session summary line when at least one item failed.
	virtual std::string decorateSummaryFail(const std::string& text) const;

	/// The `#` fill characters inside the progress bar.
	virtual std::string decorateBarFill(int count) const;

	/// The `.` empty characters inside the progress bar.
	virtual std::string decorateBarEmpty(int count) const;

	/// The outer brackets `[…]` of the progress bar (inner already decorated).
	virtual std::string decorateBarOuter(const std::string& inner) const;

	/// Current step / stage label (e.g. "Downloading", "Extracting").
	virtual std::string decorateStep(const std::string& step) const;

	/// "OK" success marker on its own line after an item finishes.
	virtual std::string decorateSuccessMarker(const std::string& text) const;

	/// "[FAILED]" marker text (does NOT include the reason).
	virtual std::string decorateFailureMarker(const std::string& text) const;

	/// Generic informational message text.
	virtual std::string decorateMessage(const std::string& text) const;

private:
	// ── State ─────────────────────────────────────────────────────────────────

	DisplayMode                                        currentMode{DisplayMode::IDLE};
	std::unordered_map<std::string, DisplayItemStatus> itemMap;
	std::vector<std::string>                           tableHeaders;
	std::vector<std::vector<std::string>>              tableRows;
	std::vector<size_t>                                colWidths;
	bool                                               fieldValueTable{false};
	mutable std::mutex                                 mtx;

	// ── Private helpers ───────────────────────────────────────────────────────

	/// Renders the full `[##....] 50%` progress bar string.
	std::string renderBar(int percent) const;

	/// Returns the label string for a mode enum value.
	static std::string modeLabel(DisplayMode mode);

	/// Prints one separator rule to stdout (uses decorateRule).
	void printRule() const;

	/// Formats one item progress line (without trailing newline).
	std::string formatItemLine(const std::string& itemId,
	                           const DisplayProgressMetrics& metrics,
	                           const std::string& step) const;

	std::vector<std::string> wrapText(const std::string& text, size_t width) const;
	void renderFieldValueTable() const;
};
