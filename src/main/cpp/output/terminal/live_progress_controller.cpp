#include "output/terminal/live_progress_controller.h"

#include "output/terminal/ansi.h"
#include "output/terminal/cursor_guard.h"

#include <indicators/cursor_control.hpp>
#include "output/terminal/progress_phase.h"
#include "output/terminal/render_throttle.h"
#include "../plain_display_internal.h"

#include "output/terminal/braille_spinner.h"

#include <algorithm>
#include <sstream>

namespace reqpack::terminal {

LiveProgressController::LiveProgressController(std::ostream& stream, LineFormatter formatter)
    : stream_(stream), formatLine_(std::move(formatter)) {}

void LiveProgressController::begin_session(const std::string& bannerLine) {
	std::lock_guard<std::mutex> lock(mutex_);
	items_.clear();
	progressStarted_ = false;
	activeProgressLines_ = 0;
	spinnerGeneration_ = 0;
	suspended_ = false;
	cursorGuard_ = std::make_unique<CursorGuard>();
	stream_ << bannerLine << '\n';
	stream_.flush();
}

void LiveProgressController::write_inplace_line(const std::string& text, const bool end_with_newline) {
	write_carriage_return_erase(stream_);
	stream_ << text << kEraseLine;
	if (end_with_newline) {
		stream_ << '\n';
	}
}

void LiveProgressController::clear_active_progress_rows() {
	if (!progressStarted_ || activeProgressLines_ <= 0) {
		return;
	}
	if (activeProgressLines_ == 1) {
		write_carriage_return_erase(stream_);
		stream_ << kEraseLine;
		return;
	}
	move_cursor_up(stream_, activeProgressLines_);
	for (int i = 0; i < activeProgressLines_; ++i) {
		write_carriage_return_erase(stream_);
		stream_ << kEraseLine;
		if (i + 1 < activeProgressLines_) {
			stream_ << '\n';
		}
	}
}

void LiveProgressController::end_session(const bool /*success*/,
                                       const int /*succeeded*/,
                                       const int /*skipped*/,
                                       const int /*failed*/,
                                       const std::string& summaryLine) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		clear_active_progress_rows();
		activeProgressLines_ = 0;
		progressStarted_ = false;
	}
	if (!summaryLine.empty()) {
		stream_ << summaryLine << '\n';
	}
	stream_.flush();
	cursorGuard_.reset();
}

void LiveProgressController::begin_item(const std::string& itemId, const std::string& label) {
	std::lock_guard<std::mutex> lock(mutex_);
	items_[itemId] = ItemRuntime{
	    .status = DisplayItemStatus{
	        .id = itemId,
	        .label = label,
	        .metrics = DisplayProgressMetrics{.percent = 0},
	        .step = "starting",
	        .state = DisplayItemState::RUNNING,
	    },
	};
}

void LiveProgressController::note_step(const std::string& itemId, const std::string& step) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = items_.find(itemId);
	if (it == items_.end()) {
		items_[itemId] = ItemRuntime{.status = {.id = itemId, .label = itemId, .state = DisplayItemState::RUNNING}};
		it = items_.find(itemId);
	}
	it->second.status.step = step;
	it->second.status.state = DisplayItemState::RUNNING;
}

void LiveProgressController::merge_metrics(ItemRuntime& item, const DisplayProgressMetrics& update) {
	plain_display_internal::merge_progress_metrics(item.status.metrics, update);
	const ProgressPhase phase = classify_progress_step(item.status.step);
	if (update.bytesPerSecond.has_value() && show_transfer_metrics(phase)) {
		item.speedEma.observe(update.bytesPerSecond.value());
		item.status.metrics.bytesPerSecond = item.speedEma.value();
	} else if (!show_transfer_metrics(phase)) {
		item.status.metrics.bytesPerSecond.reset();
	}
	item.status.metrics = canonicalize_progress_metrics(item.status.metrics, resolve_progress_percent(item.status.metrics));
}

void LiveProgressController::update_item(const std::string& itemId,
                                         const DisplayProgressMetrics& metrics,
                                         const bool forceRender) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = items_.find(itemId);
	if (it == items_.end()) {
		items_[itemId] = ItemRuntime{.status = {.id = itemId, .label = itemId, .state = DisplayItemState::RUNNING}};
		it = items_.find(itemId);
	}
	merge_metrics(it->second, metrics);
	it->second.status.state = DisplayItemState::RUNNING;
	const auto now = std::chrono::steady_clock::now();
	const bool phaseChange = metrics.bytesPerSecond.has_value();
	const bool complete = it->second.status.metrics.percent.has_value() && it->second.status.metrics.percent.value() >= 100;
	if (!should_render(now, it->second.lastRender, kDefaultRenderInterval, forceRender || phaseChange || complete)) {
		return;
	}
	++spinnerGeneration_;
	redraw_progress(forceRender || complete);
}

void LiveProgressController::set_step(const std::string& itemId, const std::string& step, const bool forceRender) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = items_.find(itemId);
	if (it == items_.end()) {
		items_[itemId] = ItemRuntime{.status = {.id = itemId, .label = itemId, .state = DisplayItemState::RUNNING}};
		it = items_.find(itemId);
	}
	const ProgressPhase oldPhase = classify_progress_step(it->second.status.step);
	it->second.status.step = step;
	const ProgressPhase newPhase = classify_progress_step(step);
	if (newPhase != ProgressPhase::TRANSFER) {
		it->second.speedEma.reset();
		it->second.status.metrics.bytesPerSecond.reset();
	}
	const bool force = forceRender || oldPhase != newPhase;
	const auto now = std::chrono::steady_clock::now();
	if (!should_render(now, it->second.lastRender, kDefaultRenderInterval, force)) {
		return;
	}
	++spinnerGeneration_;
	redraw_progress(force);
}

std::vector<std::string> LiveProgressController::running_item_ids() const {
	std::vector<std::string> ids;
	for (const auto& [id, runtime] : items_) {
		if (runtime.status.state == DisplayItemState::RUNNING) {
			ids.push_back(id);
		}
	}
	std::sort(ids.begin(), ids.end());
	return ids;
}

void LiveProgressController::suspend_for_external_output(const std::string& statusLine) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (suspended_) {
		return;
	}
	clear_active_progress_rows();
	activeProgressLines_ = 0;
	progressStarted_ = false;
	if (!statusLine.empty()) {
		stream_ << statusLine << '\n';
	}
	indicators::show_console_cursor(true);
	stream_.flush();
	suspended_ = true;
}

void LiveProgressController::resume_after_external_output() {
	std::vector<std::string> deferredOutcomeLines;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!suspended_) {
			return;
		}
		suspended_ = false;
		for (auto& [itemId, runtime] : items_) {
			if (!runtime.pendingOutcomeLine.empty()) {
				deferredOutcomeLines.push_back(runtime.pendingOutcomeLine);
				runtime.pendingOutcomeLine.clear();
			}
		}
	}
	for (const std::string& line : deferredOutcomeLines) {
		stream_ << line << '\n';
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (cursorGuard_) {
			indicators::show_console_cursor(false);
		}
		if (running_item_ids().empty()) {
			stream_.flush();
			return;
		}
		progressStarted_ = false;
		activeProgressLines_ = 0;
		redraw_progress(true);
	}
}

void LiveProgressController::redraw_progress(const bool /*force*/) {
	if (suspended_) {
		return;
	}
	const std::vector<std::string> running = running_item_ids();
	if (running.empty()) {
		return;
	}

	if (running.size() == 1) {
		const ItemRuntime& runtime = items_.at(running.front());
		const std::string_view spinner = braille_spinner_frame(spinnerGeneration_ + runtime.spinnerTick);
		const std::string line = formatLine_(running.front(), runtime.status, spinner, style_);
		if (progressStarted_ && activeProgressLines_ == 1) {
			write_inplace_line(line, false);
		} else {
			write_inplace_line(line, false);
		}
		activeProgressLines_ = 1;
		progressStarted_ = true;
		stream_.flush();
		return;
	}

	if (progressStarted_ && activeProgressLines_ > 0) {
		move_cursor_up(stream_, activeProgressLines_);
	}
	for (std::size_t index = 0; index < running.size(); ++index) {
		const std::string& id = running[index];
		const ItemRuntime& runtime = items_.at(id);
		const std::string_view spinner = braille_spinner_frame(spinnerGeneration_ + runtime.spinnerTick);
		const std::string line = formatLine_(id, runtime.status, spinner, style_);
		write_carriage_return_erase(stream_);
		stream_ << line << kEraseLine;
		if (index + 1 < running.size()) {
			stream_ << '\n';
		}
	}
	activeProgressLines_ = static_cast<int>(running.size());
	progressStarted_ = true;
	stream_.flush();
}

void LiveProgressController::complete_item(const std::string& itemId, const bool /*success*/, const std::string& markerLine) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = items_.find(itemId);
		if (it != items_.end() && it->second.outcomePrinted) {
			return;
		}
		if (it != items_.end()) {
			it->second.outcomePrinted = true;
			it->second.status.state = DisplayItemState::SUCCESS;
			it->second.status.metrics.percent = 100;
			it->second.status.step = "done";
		}
		if (suspended_) {
			if (it != items_.end() && !markerLine.empty()) {
				it->second.pendingOutcomeLine = markerLine;
			} else if (!markerLine.empty()) {
				stream_ << markerLine << '\n';
			}
			return;
		}
		if (progressStarted_ && activeProgressLines_ > 0) {
			if (activeProgressLines_ == 1) {
				write_inplace_line(markerLine, true);
			} else {
				clear_active_progress_rows();
				stream_ << markerLine << kEraseLine << '\n';
			}
			activeProgressLines_ = 0;
			progressStarted_ = false;
		} else if (!markerLine.empty()) {
			stream_ << markerLine << '\n';
		}
	}
	stream_.flush();
}

void LiveProgressController::skip_item(const std::string& itemId, const std::string& line) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = items_.find(itemId);
		if (it != items_.end()) {
			it->second.status.state = DisplayItemState::SKIPPED;
		}
	}
	if (!line.empty()) {
		stream_ << line << '\n';
	}
	stream_.flush();
}

void LiveProgressController::fail_item(const std::string& itemId, const std::string& line) {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = items_.find(itemId);
		if (it != items_.end()) {
			it->second.status.state = DisplayItemState::FAILED;
		}
		if (progressStarted_ && activeProgressLines_ > 0) {
			clear_active_progress_rows();
			activeProgressLines_ = 0;
			progressStarted_ = false;
		}
	}
	if (!line.empty()) {
		stream_ << line << '\n';
	}
	stream_.flush();
}

void LiveProgressController::prepare_static_output() {
	std::lock_guard<std::mutex> lock(mutex_);
	clear_active_progress_rows();
	activeProgressLines_ = 0;
	progressStarted_ = false;
	cursorGuard_.reset();
	stream_.flush();
}

void LiveProgressController::log_message(const std::string& line) {
	if (line.empty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (progressStarted_ && activeProgressLines_ > 0) {
			if (activeProgressLines_ == 1) {
				write_inplace_line(std::string{}, true);
			} else {
				clear_active_progress_rows();
				stream_ << '\n';
			}
		}
	}
	stream_ << line << '\n';
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (running_item_ids().empty()) {
			return;
		}
		progressStarted_ = false;
		activeProgressLines_ = 0;
		redraw_progress(true);
	}
}

void LiveProgressController::flush() {
	stream_.flush();
}

}  // namespace reqpack::terminal
