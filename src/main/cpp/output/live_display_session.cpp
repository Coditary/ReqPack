#include "output/live_display.h"

#include "output/terminal/block_progress_bar.h"
#include "output/terminal/progress_phase.h"
#include "output/terminal/tty.h"

#include "plain_display_internal.h"

#include <sstream>

namespace {

bool has_measurable_progress(const DisplayProgressMetrics& metrics) {
	return metrics.percent.has_value() || metrics.currentBytes.has_value() || metrics.totalBytes.has_value();
}

}  // namespace

LiveDisplay::LiveDisplay() {
	if (reqpack::terminal::use_animated_terminal_ui()) {
		live_ = std::make_unique<reqpack::terminal::LiveProgressController>(
		    out(),
		    [this](const std::string& itemId,
		           const DisplayItemStatus& status,
		           const std::string_view spinnerFrame,
		           const reqpack::terminal::LiveProgressLineStyle& /*style*/) {
			    return format_live_progress_line(itemId, status, spinnerFrame);
		    });
	}
}

bool LiveDisplay::use_live_ui() const {
	return live_ != nullptr;
}

std::string LiveDisplay::decorateSpinnerFrame(const std::string_view frame) const {
	return std::string(frame);
}

std::string LiveDisplay::format_session_banner(const DisplayMode mode, const std::vector<std::string>& items) const {
	const std::string joined = plain_display_internal::join_items(items);
	std::ostringstream banner;
	banner << "📦 ";
	switch (mode) {
		case DisplayMode::SEARCH:
			if (!joined.empty()) {
				banner << "Searching " << joined;
			} else {
				banner << "Searching";
			}
			break;
		case DisplayMode::INFO:
			if (!joined.empty()) {
				banner << "Info for " << joined;
			} else {
				banner << "Showing package info";
			}
			break;
		case DisplayMode::LIST:
			banner << (joined.empty() ? "Listing packages" : "Listing " + joined);
			break;
		case DisplayMode::INSTALL:
			banner << (joined.empty() ? "Installing" : "Installing " + joined);
			break;
		case DisplayMode::REMOVE:
			banner << (joined.empty() ? "Removing" : "Removing " + joined);
			break;
		case DisplayMode::UPDATE:
			banner << (joined.empty() ? "Updating" : "Updating " + joined);
			break;
		default:
			banner << PlainDisplay::modeLabel(mode);
			if (!joined.empty()) {
				banner << ' ' << joined;
			}
			break;
	}
	banner << "...";
	return banner.str();
}

std::string LiveDisplay::format_live_progress_line(const std::string& itemId,
                                                   const DisplayItemStatus& status,
                                                   const std::string_view spinnerFrame) const {
	const reqpack::terminal::ProgressPhase phase = reqpack::terminal::classify_progress_step(status.step);
	const DisplayProgressMetrics metrics =
	    reqpack::terminal::metrics_for_display_phase(status.metrics, phase);
	const bool measurable = has_measurable_progress(status.metrics);
	const int percent = measurable ? resolve_progress_percent(metrics).value_or(0) : 0;
	const double ratio = measurable ? static_cast<double>(percent) / 100.0 : 0.0;

	std::ostringstream line;
	line << decorateSpinnerFrame(spinnerFrame) << ' ' << itemId << ' ';
	if (measurable) {
		line << reqpack::terminal::render_fractional_block_bar(ratio, PlainDisplay::BAR_WIDTH) << ' '
		     << reqpack::terminal::format_progress_tail(metrics, phase);
	}
	if (!status.step.empty()) {
		line << "  " << decorateStep(status.step);
	}
	return line.str();
}

std::string LiveDisplay::format_session_summary(const bool success,
                                                const int succeeded,
                                                const int skipped,
                                                const int failed) const {
	switch (sessionMode_) {
		case DisplayMode::INFO:
			return {};
		case DisplayMode::SEARCH:
		case DisplayMode::LIST:
		case DisplayMode::OUTDATED:
			return {};
		default:
			break;
	}

	std::ostringstream stream;
	if (succeeded > 0 || skipped > 0 || failed > 0) {
		const char* verb = "updated";
		const char* emoji = "✨";
		switch (sessionMode_) {
			case DisplayMode::INSTALL:
				verb = "installed";
				emoji = "✨";
				break;
			case DisplayMode::REMOVE:
				verb = "removed";
				emoji = "🗑️";
				break;
			case DisplayMode::UPDATE:
			default:
				verb = "updated";
				emoji = "✨";
				break;
		}
		stream << emoji << ' ' << succeeded << ' ' << verb << ", " << skipped << " skipped, " << failed << " failed";
		if (!success) {
			return decorateSummaryFail(stream.str());
		}
		return decorateSummaryOk(stream.str());
	}
	if (sessionMode_ == DisplayMode::SEARCH || sessionMode_ == DisplayMode::LIST) {
		return {};
	}
	const std::string fallback = success ? "done" : "failed";
	return success ? decorateSummaryOk(fallback) : decorateSummaryFail(fallback);
}

std::string LiveDisplay::format_log_branch(const std::string& text, const std::string& source) const {
	const std::string trimmed = plain_display_internal::trim_trailing_newlines(text);
	std::ostringstream line;
	line << "  ├─ ";
	if (trimmed.rfind("already on release", 0) == 0) {
		line << "ℹ " << decorateMessage(trimmed);
		return line.str();
	}
	if (trimmed.rfind("link:", 0) == 0 || trimmed.rfind("now on release", 0) == 0) {
		line << "🔗 " << decorateMessage(trimmed);
		return line.str();
	}
	if (!source.empty()) {
		line << '[' << source << "] ";
	}
	line << decorateMessage(trimmed);
	return line.str();
}

std::string LiveDisplay::format_item_outcome_line(const std::string& /*itemId*/, const std::string& body) const {
	std::ostringstream line;
	line << "  └─ " << body;
	return line.str();
}

std::string LiveDisplay::format_item_step_line(const std::string& itemId, const std::string& step) const {
	std::ostringstream line;
	line << "  ├─ " << decorateSpinnerFrame("⠿") << ' ' << itemId;
	if (!step.empty()) {
		line << "  " << decorateStep(step);
	}
	return line.str();
}

void LiveDisplay::onSessionBegin(const DisplayMode mode, const std::vector<std::string>& items) {
	sessionMode_ = mode;
	sessionItems_ = items;
	setSessionContext(mode, items);
	if (use_live_ui()) {
		if (mode != DisplayMode::LIST && mode != DisplayMode::SEARCH && mode != DisplayMode::OUTDATED
		    && mode != DisplayMode::INFO) {
			live_->begin_session(format_session_banner(mode, items));
		}
		return;
	}
	PlainDisplay::onSessionBegin(mode, items);
}

void LiveDisplay::onSessionEnd(const bool success, const int succeeded, const int skipped, const int failed) {
	if (use_live_ui()) {
		const std::string summary = format_session_summary(success, succeeded, skipped, failed);
		live_->end_session(success, succeeded, skipped, failed, summary);
		return;
	}
	PlainDisplay::onSessionEnd(success, succeeded, skipped, failed);
}

void LiveDisplay::onItemBegin(const std::string& itemId, const std::string& label) {
	if (use_live_ui()) {
		trackItemBegin(itemId, label);
		live_->begin_item(itemId, label);
		return;
	}
	PlainDisplay::onItemBegin(itemId, label);
}

void LiveDisplay::onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) {
	if (use_live_ui()) {
		live_->update_item(itemId, metrics, false);
		return;
	}
	PlainDisplay::onItemProgress(itemId, metrics);
}

void LiveDisplay::onItemStep(const std::string& itemId, const std::string& step) {
	const DisplayProgressMetrics metrics = trackItemStep(itemId, step);
	if (use_live_ui()) {
		live_->note_step(itemId, step);
		if (!has_measurable_progress(metrics)) {
			live_->log_message(format_item_step_line(itemId, step));
			return;
		}
		live_->set_step(itemId, step, true);
		return;
	}
	PlainDisplay::onItemStep(itemId, step);
}

void LiveDisplay::onItemSuccess(const std::string& itemId) {
	if (use_live_ui()) {
		if (!tryMarkItemSuccess(itemId)) {
			return;
		}
		const std::string marker =
		    format_item_outcome_line(itemId, decorateSuccessMarker(std::string("✔ ") + itemId + " done"));
		live_->complete_item(itemId, true, marker);
		return;
	}
	PlainDisplay::onItemSuccess(itemId);
}

void LiveDisplay::onItemSkipped(const std::string& itemId, const std::string& reason) {
	if (use_live_ui()) {
		std::ostringstream line;
		line << "  ├─ " << decorateSkippedMarker("skipped " + itemId);
		if (!reason.empty()) {
			line << "  " << decorateStep(reason);
		}
		live_->skip_item(itemId, line.str());
		return;
	}
	PlainDisplay::onItemSkipped(itemId, reason);
}

void LiveDisplay::onItemFailure(const std::string& itemId, const std::string& reason) {
	if (use_live_ui()) {
		std::ostringstream line;
		line << "  ├─ " << decorateFailureMarker("✖ " + itemId);
		if (!reason.empty()) {
			line << "  " << reason;
		}
		live_->fail_item(itemId, line.str());
		return;
	}
	PlainDisplay::onItemFailure(itemId, reason);
}

void LiveDisplay::onMessage(const std::string& text, const std::string& source) {
	if (use_live_ui()) {
		live_->log_message(format_log_branch(text, source));
		return;
	}
	PlainDisplay::onMessage(text, source);
}

void LiveDisplay::onUnindentedText(const std::string& text) {
	if (use_live_ui()) {
		live_->prepare_static_output();
	}
	PlainDisplay::onUnindentedText(text);
}

void LiveDisplay::onTableEnd() {
	if (use_live_ui()) {
		live_->prepare_static_output();
	}
	PlainDisplay::onTableEnd();
}

void LiveDisplay::flush() {
	if (use_live_ui()) {
		live_->flush();
		return;
	}
	PlainDisplay::flush();
}

void LiveDisplay::onProgressSuspend() {
	if (use_live_ui()) {
		live_->suspend_for_external_output();
	}
}

void LiveDisplay::onProgressResume() {
	if (use_live_ui()) {
		live_->resume_after_external_output();
	}
}
