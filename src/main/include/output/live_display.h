#pragma once

#include "output/plain_display.h"
#include "output/terminal/live_progress_controller.h"

#include <memory>
#include <optional>
#include <string_view>

// ─────────────────────────────────────────────────────────────────────────────
// LiveDisplay — TTY progress UI with in-place rendering (indicators cursor API).
//
// Falls back to PlainDisplay line output when stdout is not a TTY.
// ─────────────────────────────────────────────────────────────────────────────
class LiveDisplay : public PlainDisplay {
public:
	LiveDisplay();

	// Tighter than PlainDisplay::LABEL_WIDTH for live progress rows.
	static constexpr int LIVE_LABEL_WIDTH = 10;

protected:
	bool use_live_ui() const;

	virtual std::string decorateSpinnerFrame(std::string_view frame) const;
	virtual std::string format_live_progress_line(const std::string& itemId,
	                                              const DisplayItemStatus& status,
	                                              std::string_view spinnerFrame) const;

	virtual std::string format_session_summary(bool success, int succeeded, int skipped, int failed) const;
	virtual std::string format_log_branch(const std::string& text, const std::string& source) const;
	virtual std::string format_item_outcome_line(const std::string& itemId, const std::string& body) const;
	virtual std::string format_item_step_line(const std::string& itemId, const std::string& step) const;

	void onSessionBegin(DisplayMode mode, const std::vector<std::string>& items) override;
	void onSessionEnd(bool success, int succeeded, int skipped, int failed) override;
	void onItemBegin(const std::string& itemId, const std::string& label) override;
	void onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) override;
	void onItemStep(const std::string& itemId, const std::string& step) override;
	void onItemSuccess(const std::string& itemId) override;
	void onItemSkipped(const std::string& itemId, const std::string& reason) override;
	void onItemFailure(const std::string& itemId, const std::string& reason) override;
	void onMessage(const std::string& text, const std::string& source) override;
	void onUnindentedText(const std::string& text) override;
	void onTableEnd() override;
	void flush() override;
	void onProgressSuspend() override;
	void onProgressResume() override;

private:
	std::unique_ptr<reqpack::terminal::LiveProgressController> live_;
	DisplayMode sessionMode_{DisplayMode::IDLE};
	std::vector<std::string> sessionItems_;

	std::string format_session_banner(DisplayMode mode, const std::vector<std::string>& items) const;
};
