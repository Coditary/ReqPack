#include "output/plain_display.h"

#include "plain_display_internal.h"
#include "output/logger.h"

#include <iomanip>
#include <optional>

void PlainDisplay::setSessionContext(const DisplayMode mode, const std::vector<std::string>& items) {
	std::lock_guard<std::mutex> lock(mtx);
	currentMode = mode;
	itemMap.clear();
	for (const auto& id : items) {
		itemMap[id] = DisplayItemStatus{.id = id, .label = id};
	}
}

void PlainDisplay::trackItemBegin(const std::string& itemId, const std::string& label) {
	std::lock_guard<std::mutex> lock(mtx);
	itemMap[itemId] = DisplayItemStatus{
	    .id = itemId,
	    .label = label,
	    .metrics = DisplayProgressMetrics{.percent = 0},
	    .step = "starting",
	    .state = DisplayItemState::RUNNING,
	};
}

DisplayProgressMetrics PlainDisplay::trackItemStep(const std::string& itemId, const std::string& step) {
	std::lock_guard<std::mutex> lock(mtx);
	DisplayItemStatus& status = itemMap[itemId];
	if (status.id.empty()) {
		status.id = itemId;
		status.label = itemId;
	}
	status.step = step;
	status.state = DisplayItemState::RUNNING;
	return status.metrics;
}

bool PlainDisplay::tryMarkItemSuccess(const std::string& itemId) {
	std::lock_guard<std::mutex> lock(mtx);
	auto it = itemMap.find(itemId);
	if (it != itemMap.end() && it->second.state == DisplayItemState::SUCCESS) {
		return false;
	}
	if (it != itemMap.end()) {
		it->second.state = DisplayItemState::SUCCESS;
		it->second.step = "done";
	}
	return true;
}

void PlainDisplay::onSessionBegin(DisplayMode mode, const std::vector<std::string>& items) {
    setSessionContext(mode, items);
    if (mode == DisplayMode::LIST || mode == DisplayMode::SEARCH || mode == DisplayMode::OUTDATED
        || mode == DisplayMode::INFO) {
        return;
    }
    std::lock_guard<std::mutex> lock(mtx);

    out() << decorateActionBanner(plain_display_internal::action_banner_text(mode, items)) << '\n';
    out().flush();
}

void PlainDisplay::onSessionEnd(bool success, int succeeded, int skipped, int failed) {
    std::lock_guard<std::mutex> lock(mtx);

    const std::optional<std::string> summary =
        plain_display_internal::session_summary_text(currentMode, success, succeeded, skipped, failed);
    if (!summary.has_value()) {
        return;
    }

    out() << '\n';
    const bool failStyle = failed > 0 || !success;
    if (failStyle) {
        out() << decorateSummaryFail(*summary) << '\n';
    } else {
        out() << decorateSummaryOk(*summary) << '\n';
    }
    out().flush();
}

void PlainDisplay::onItemBegin(const std::string& itemId, const std::string& label) {
    trackItemBegin(itemId, label);
    if (Logger::instance().quietTransactionTranscript()) {
        return;
    }
    out() << formatItemLine(itemId, DisplayProgressMetrics{.percent = 0}, "starting") << '\n';
    out().flush();
}

void PlainDisplay::onItemProgress(const std::string& itemId, const DisplayProgressMetrics& metrics) {
    DisplayProgressMetrics displayMetrics;
    std::string step;
    {
        std::lock_guard<std::mutex> lock(mtx);
        DisplayItemStatus& status = itemMap[itemId];
        if (status.id.empty()) {
            status.id = itemId;
            status.label = itemId;
        }
        plain_display_internal::merge_progress_metrics(status.metrics, metrics);
        status.metrics = canonicalize_progress_metrics(status.metrics, resolve_progress_percent(status.metrics));
        status.state = DisplayItemState::RUNNING;
        step = status.step;
        displayMetrics = status.metrics;
    }
    if (Logger::instance().quietTransactionTranscript()
        && !plain_display_internal::progress_has_byte_transfer(displayMetrics)) {
        return;
    }
    out() << formatItemLine(itemId, displayMetrics, step) << '\n';
    out().flush();
}

void PlainDisplay::onItemStep(const std::string& itemId, const std::string& step) {
    if (Logger::instance().quietTransactionTranscript()) {
        std::lock_guard<std::mutex> lock(mtx);
        DisplayItemStatus& status = itemMap[itemId];
        if (status.id.empty()) {
            status.id = itemId;
            status.label = itemId;
        }
        status.step = step;
        status.state = DisplayItemState::RUNNING;
        return;
    }
    DisplayProgressMetrics metrics;
    {
        std::lock_guard<std::mutex> lock(mtx);
        DisplayItemStatus& status = itemMap[itemId];
        if (status.id.empty()) {
            status.id = itemId;
            status.label = itemId;
        }
        status.step = step;
        status.state = DisplayItemState::RUNNING;
        metrics = status.metrics;
    }
    out() << formatItemLine(itemId, metrics, step) << '\n';
    out().flush();
}

void PlainDisplay::onItemSuccess(const std::string& itemId, const std::string& version) {
    bool alreadySucceeded = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = itemMap.find(itemId);
        if (it != itemMap.end()) {
            alreadySucceeded = it->second.state == DisplayItemState::SUCCESS;
            it->second.metrics.percent = 100;
            if (it->second.metrics.totalBytes.has_value()) {
                it->second.metrics.currentBytes = it->second.metrics.totalBytes;
            }
            it->second.metrics = canonicalize_progress_metrics(it->second.metrics, 100);
            it->second.step = "done";
            it->second.state = DisplayItemState::SUCCESS;
            if (!version.empty()) {
                it->second.successVersion = version;
            }
        }
    }
    if (alreadySucceeded) {
        return;
    }
    std::ostringstream body;
    body << itemId;
    if (!version.empty()) {
        body << ' ' << decorateSuccessVersion('(' + version + ')');
    }
    body << ' ' << plain_display_internal::item_outcome_phrase(currentMode);
    const std::string line = plain_display_internal::format_item_action_line(
        plain_display_internal::kSuccessMark, decorateSuccessMarker(body.str()));
    out() << line << '\n';
    out().flush();
}

void PlainDisplay::onItemFailure(const std::string& itemId, const std::string& reason) {
    std::string effectiveReason = reason;
    if (!commitItemFailure(itemId, reason, effectiveReason)) {
        return;
    }
    writeItemFailureLines(itemId, effectiveReason);
}

bool PlainDisplay::commitItemFailure(const std::string& itemId, const std::string& reason,
                                     std::string& effectiveReason) {
    effectiveReason = reason;
    bool alreadyFailed = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = itemMap.find(itemId);
        if (it != itemMap.end()) {
            alreadyFailed = it->second.state == DisplayItemState::FAILED;
            if (alreadyFailed) {
                if (!effectiveReason.empty() && effectiveReason == it->second.failureReason) {
                    return false;
                }
                if (plain_display_internal::is_generic_plugin_failure_summary(effectiveReason)
                    && !it->second.failureReason.empty()) {
                    return false;
                }
                if (!effectiveReason.empty() && !plain_display_internal::is_generic_plugin_failure_summary(effectiveReason)) {
                    it->second.failureReason = effectiveReason;
                }
            } else {
                it->second.state = DisplayItemState::FAILED;
                if (!effectiveReason.empty()) {
                    it->second.failureReason = effectiveReason;
                }
            }
        } else if (!effectiveReason.empty()) {
            itemMap[itemId] = DisplayItemStatus {.id = itemId, .label = itemId, .state = DisplayItemState::FAILED, .failureReason = effectiveReason};
        }
    }
    if (alreadyFailed && plain_display_internal::is_generic_plugin_failure_summary(effectiveReason)) {
        return false;
    }
    if (alreadyFailed && effectiveReason.empty()) {
        return false;
    }
    if (plain_display_internal::is_generic_plugin_failure_summary(effectiveReason)) {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = itemMap.find(itemId);
        if (it != itemMap.end() && !it->second.failureReason.empty()) {
            effectiveReason = it->second.failureReason;
        } else if (alreadyFailed) {
            return false;
        }
    }
    return true;
}

void PlainDisplay::writeItemFailureLines(const std::string& itemId, const std::string& effectiveReason) {
    const std::string primary = plain_display_internal::format_item_failure_primary(itemId, effectiveReason);
    const std::string line = plain_display_internal::format_item_action_line(
        plain_display_internal::kErrorMark, decorateFailureMarker(primary));
    out() << line << '\n';
    if (const std::optional<std::string> hint =
            plain_display_internal::failure_hint_for_reason(itemId, effectiveReason);
        hint.has_value()) {
        out() << plain_display_internal::kIndent << plain_display_internal::kIndent
              << decorateFailureHint("hint: " + hint.value()) << '\n';
    }
    out().flush();
}

void PlainDisplay::onItemSkipped(const std::string& itemId, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        DisplayItemStatus& status = itemMap[itemId];
        if (status.id.empty()) {
            status.id = itemId;
            status.label = itemId;
        }
        status.step = reason.empty() ? "already installed" : reason;
        status.state = DisplayItemState::SKIPPED;
    }
    std::ostringstream body;
    body << itemId;
    if (reason == "already installed") {
        body << " is already installed, skipping";
    } else if (!reason.empty()) {
        body << "  " << reason;
    } else {
        body << " skipped";
    }
    const std::string line = plain_display_internal::format_item_action_line(
        plain_display_internal::kSkippedMark, decorateSkippedMarker(body.str()));
    out() << line << '\n';
    out().flush();
}

void PlainDisplay::onMessage(const std::string& text, const std::string& source) {
    if (Logger::instance().quietTransactionTranscript() && !source.empty() && source.find(':') != std::string::npos) {
        const std::string trimmed = plain_display_internal::trim_trailing_newlines(text);
        if (trimmed.rfind("Cause:", 0) == 0 || trimmed.rfind("Fix:", 0) == 0) {
            return;
        }
    }
    if (!source.empty()) {
        out() << plain_display_internal::kIndent << '[' << source << "]  " << decorateMessage(text) << '\n';
    } else {
        out() << plain_display_internal::kIndent << decorateMessage(text) << '\n';
    }
    out().flush();
}

void PlainDisplay::onUnindentedText(const std::string& text) {
    out() << text;
    if (!text.empty() && text.back() != '\n') {
        out() << '\n';
    }
    out().flush();
}
