#include "output/plain_display.h"

#include "plain_display_internal.h"

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

    std::string header = modeLabel(mode);
    if (!items.empty()) {
        header += ": " + plain_display_internal::join_items(items);
    }

    printRule();
    out() << "  " << decorateHeader(header) << '\n';
    printRule();
    out().flush();
}

void PlainDisplay::onSessionEnd(bool success, int succeeded, int skipped, int failed) {
    std::lock_guard<std::mutex> lock(mtx);

    const std::optional<std::string> summary =
        plain_display_internal::session_summary_text(currentMode, success, succeeded, skipped, failed);
    if (!summary.has_value()) {
        return;
    }

    printRule();
    const bool failStyle = failed > 0 || !success;
    if (failStyle) {
        out() << "  " << decorateSummaryFail(*summary) << '\n';
    } else {
        out() << "  " << decorateSummaryOk(*summary) << '\n';
    }
    printRule();
    out().flush();
}

void PlainDisplay::onItemBegin(const std::string& itemId, const std::string& label) {
    trackItemBegin(itemId, label);
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
    out() << formatItemLine(itemId, displayMetrics, step) << '\n';
    out().flush();
}

void PlainDisplay::onItemStep(const std::string& itemId, const std::string& step) {
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

void PlainDisplay::onItemSuccess(const std::string& itemId) {
    bool alreadySucceeded = false;
    DisplayProgressMetrics metrics{.percent = 100};
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
            metrics = it->second.metrics;
        }
    }
    if (alreadySucceeded) {
        return;
    }
    out() << formatItemLine(itemId, metrics, "done") << '\n';
    out() << "  " << std::left << std::setw(LABEL_WIDTH) << itemId << "  " << decorateSuccessMarker("OK") << '\n';
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
    out() << "  " << std::left << std::setw(LABEL_WIDTH) << itemId << "  " << decorateSkippedMarker("SKIPPED");
    if (!reason.empty()) {
        out() << "  " << decorateStep(reason);
    }
    out() << '\n';
    out().flush();
}

void PlainDisplay::onItemFailure(const std::string& itemId, const std::string& reason) {
    bool alreadyFailed = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = itemMap.find(itemId);
        if (it != itemMap.end()) {
            alreadyFailed = it->second.state == DisplayItemState::FAILED;
            it->second.state = DisplayItemState::FAILED;
        }
    }
    if (alreadyFailed) {
        return;
    }
    out() << "  " << std::left << std::setw(LABEL_WIDTH) << itemId << "  " << decorateFailureMarker("[FAILED]");
    if (!reason.empty()) {
        out() << "  " << reason;
    }
    out() << '\n';
    out().flush();
}

void PlainDisplay::onMessage(const std::string& text, const std::string& source) {
    if (!source.empty()) {
        out() << "  [" << source << "]  " << decorateMessage(text) << '\n';
    } else {
        out() << "  " << decorateMessage(text) << '\n';
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
