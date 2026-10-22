#pragma once

#include "output/idisplay.h"
#include "output/terminal/cursor_guard.h"
#include "output/terminal/speed_ema.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>

namespace reqpack::terminal {

struct LiveProgressLineStyle {
	int barWidth{20};
};

/// In-place multi-item progress rendering (indicators-backed cursor control).
class LiveProgressController {
public:
	using LineFormatter = std::function<std::string(
	    const std::string& itemId,
	    const DisplayItemStatus& status,
	    std::string_view spinnerFrame,
	    const LiveProgressLineStyle& style)>;

	explicit LiveProgressController(std::ostream& stream, LineFormatter formatter);

	void begin_session(const std::string& bannerLine);
	void end_session(bool success, int succeeded, int skipped, int failed, const std::string& summaryLine);

	void begin_item(const std::string& itemId, const std::string& label);
	void update_item(const std::string& itemId, const DisplayProgressMetrics& metrics, bool forceRender);
	void note_step(const std::string& itemId, const std::string& step);
	void set_step(const std::string& itemId, const std::string& step, bool forceRender);
	void complete_item(const std::string& itemId, bool success, const std::string& markerLine);
	void skip_item(const std::string& itemId, const std::string& line);
	void fail_item(const std::string& itemId, const std::string& line);

	void log_message(const std::string& line);
	/// Clear in-place progress and restore cursor before full-width static output.
	void prepare_static_output();
	void suspend_for_external_output(const std::string& statusLine = {});
	void resume_after_external_output();
	void flush();

private:
	struct ItemRuntime {
		DisplayItemStatus status;
		SpeedEma speedEma;
		std::chrono::steady_clock::time_point lastRender{};
		std::size_t spinnerTick{0};
		bool outcomePrinted{false};
		std::string pendingOutcomeLine;
	};

	std::ostream& stream_;
	LineFormatter formatLine_;
	LiveProgressLineStyle style_;
	std::mutex mutex_;
	std::unordered_map<std::string, ItemRuntime> items_;
	std::size_t spinnerGeneration_{0};
	bool progressStarted_{false};
	int activeProgressLines_{0};
	bool suspended_{false};
	std::unique_ptr<CursorGuard> cursorGuard_;

	void merge_metrics(ItemRuntime& item, const DisplayProgressMetrics& update);
	void redraw_progress(bool force);
	void write_inplace_line(const std::string& text, bool end_with_newline);
	void clear_active_progress_rows();
	std::vector<std::string> running_item_ids() const;
};

}  // namespace reqpack::terminal
