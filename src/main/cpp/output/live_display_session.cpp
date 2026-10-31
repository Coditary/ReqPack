#include "output/live_display.h"

#include "output/terminal/block_progress_bar.h"
#include "output/terminal/progress_phase.h"
#include "output/terminal/tty.h"

#include "plain_display_internal.h"

#include "output/logger.h"

#include <sstream>

namespace {

bool has_measurable_progress(const DisplayProgressMetrics& metrics) {
	return metrics.percent.has_value() || metrics.currentBytes.has_value() || metrics.totalBytes.has_value();
}

bool use_live_progress_for(DisplayMode mode) {
	switch (mode) {
		case DisplayMode::INSTALL:
		case DisplayMode::REMOVE:
		case DisplayMode::UPDATE:
		case DisplayMode::ENSURE:
			return false;
		default:
			return true;
	}
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

bool LiveDisplay::use_animated_session_progress() const {
	return use_live_ui() && use_live_progress_for(sessionMode_);
}

std::string LiveDisplay::decorateSpinnerFrame(const std::string_view frame) const {
	return std::string(frame);
}

std::string LiveDisplay::format_session_banner(const DisplayMode mode, const std::vector<std::string>& items) const {
	return plain_display_internal::action_banner_text(mode, items);
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
	line << plain_display_internal::kIndent << decorateSpinnerFrame(spinnerFrame) << ' ' << itemId << ' ';
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
	const std::optional<std::string> summary =
	    plain_display_internal::session_summary_text(sessionMode_, success, succeeded, skipped, failed);
	if (!summary.has_value()) {
		return {};
	}
	if (!success || failed > 0) {
		return decorateSummaryFail(*summary);
	}
	return decorateSummaryOk(*summary);
}

std::string LiveDisplay::format_log_branch(const std::string& text, const std::string& source) const {
	const std::string trimmed = plain_display_internal::trim_trailing_newlines(text);
	std::ostringstream line;
	line << plain_display_internal::kIndent;
	switch (plain_display_internal::transcript_line_decoration(trimmed)) {
	case plain_display_internal::TranscriptLineDecoration::Info:
		line << plain_display_internal::kInfoMark << ' ' << decorateMessage(trimmed);
		return line.str();
	case plain_display_internal::TranscriptLineDecoration::Link:
		line << plain_display_internal::kLinkMark << ' ' << decorateMessage(trimmed);
		return line.str();
	case plain_display_internal::TranscriptLineDecoration::None:
		break;
	}
	if (!source.empty()) {
		line << '[' << source << "] ";
	}
	line << decorateMessage(trimmed);
	return line.str();
}

std::string LiveDisplay::format_item_outcome_line(const std::string& /*itemId*/, const std::string& body) const {
	return plain_display_internal::format_item_action_line(plain_display_internal::kSuccessMark, body);
}

std::string LiveDisplay::format_item_step_line(const std::string& itemId, const std::string& step) const {
	std::ostringstream line;
	line << plain_display_internal::kIndent << decorateSpinnerFrame("⠋") << ' ' << itemId;
	if (!step.empty()) {
		line << "  " << decorateStep(step);
	}
	return line.str();
}

void LiveDisplay::onSessionBegin(const DisplayMode mode, const std::vector<std::string>& items) {
	sessionMode_ = mode;
	sessionItems_ = items;
	setSessionContext(mode, items);
	if (use_live_ui() && use_live_progress_for(mode)) {
		if (mode != DisplayMode::LIST && mode != DisplayMode::SEARCH && mode != DisplayMode::OUTDATED
		    && mode != DisplayMode::INFO) {
			live_->begin_session(decorateActionBanner(format_session_banner(mode, items)));
		}
		return;
	}
	PlainDisplay::onSessionBegin(mode, items);
}

void LiveDisplay::onSessionEnd(const bool success, const int succeeded, const int skipped, const int failed) {
	if (use_animated_session_progress()) {
		const std::string summary = format_session_summary(success, succeeded, skipped, failed);
		live_->end_session(success, succeeded, skipped, failed, summary);
		return;
	}
	PlainDisplay::onSessionEnd(success, succeeded, skipped, failed);
}

void LiveDisplay::onItemBegin(const std::string& itemId, const std::string& label) {
	if (use_animated_session_progress()) {
		trackItemBegin(itemId, label);
		live_->begin_item(itemId, label);
		return;
	}
	PlainDisplay::onItemBegin(itemId, label);
}

void LiveDisplay::onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) {
	if (use_animated_session_progress()) {
		if (Logger::instance().quietTransactionTranscript()
		    && !plain_display_internal::progress_has_byte_transfer(metrics)) {
			return;
		}
		live_->update_item(itemId, metrics, false);
		return;
	}
	PlainDisplay::onItemProgress(itemId, metrics);
}

void LiveDisplay::onItemStep(const std::string& itemId, const std::string& step) {
	const DisplayProgressMetrics metrics = trackItemStep(itemId, step);
	if (use_animated_session_progress()) {
		live_->note_step(itemId, step);
		if (!has_measurable_progress(metrics)) {
			if (Logger::instance().quietTransactionTranscript()) {
				return;
			}
			live_->log_message(format_item_step_line(itemId, step));
			return;
		}
		live_->set_step(itemId, step, true);
		return;
	}
	PlainDisplay::onItemStep(itemId, step);
}

void LiveDisplay::onItemSuccess(const std::string& itemId, const std::string& version) {
	if (use_animated_session_progress()) {
		if (!tryMarkItemSuccess(itemId)) {
			return;
		}
		std::ostringstream body;
		body << itemId;
		if (!version.empty()) {
			body << ' ' << decorateSuccessVersion('(' + version + ')');
		}
		body << ' ' << plain_display_internal::item_outcome_phrase(sessionMode_);
		const std::string marker = format_item_outcome_line(itemId, decorateSuccessMarker(body.str()));
		live_->complete_item(itemId, true, marker);
		return;
	}
	PlainDisplay::onItemSuccess(itemId, version);
}

void LiveDisplay::onItemSkipped(const std::string& itemId, const std::string& reason) {
	if (use_animated_session_progress()) {
		std::ostringstream body;
		body << itemId;
		if (reason == "already installed") {
			body << " is already installed, skipping";
		} else if (!reason.empty()) {
			body << "  " << reason;
		} else {
			body << " skipped";
		}
		const std::string line =
		    plain_display_internal::format_item_action_line(plain_display_internal::kSkippedMark,
		                                                    decorateSkippedMarker(body.str()));
		live_->skip_item(itemId, line);
		return;
	}
	PlainDisplay::onItemSkipped(itemId, reason);
}

void LiveDisplay::onItemFailure(const std::string& itemId, const std::string& reason) {
	std::string effectiveReason = reason;
	if (!commitItemFailure(itemId, reason, effectiveReason)) {
		return;
	}
	if (use_animated_session_progress()) {
		const std::string primary = plain_display_internal::format_item_failure_primary(itemId, effectiveReason);
		const std::string line = plain_display_internal::format_item_action_line(
		    plain_display_internal::kErrorMark, decorateFailureMarker(primary));
		live_->fail_item(itemId, line);
		if (const std::optional<std::string> hint =
		        plain_display_internal::failure_hint_for_reason(itemId, effectiveReason);
		    hint.has_value()) {
			live_->log_message(std::string(plain_display_internal::kIndent) + plain_display_internal::kIndent
			                   + decorateFailureHint("hint: " + hint.value()));
		}
		return;
	}
	writeItemFailureLines(itemId, effectiveReason);
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
