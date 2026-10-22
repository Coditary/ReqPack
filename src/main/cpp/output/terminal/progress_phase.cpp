#include "output/terminal/progress_phase.h"

#include <algorithm>
#include <cctype>

namespace reqpack::terminal {

namespace {

std::string lower_step(std::string_view step) {
	std::string out(step);
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

bool contains_any(const std::string& haystack, const std::initializer_list<const char*> needles) {
	for (const char* needle : needles) {
		if (haystack.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

}  // namespace

ProgressPhase classify_progress_step(const std::string_view step) {
	const std::string normalized = lower_step(step);
	if (normalized.empty() || normalized == "starting" || normalized == "done") {
		return ProgressPhase::INDETERMINATE;
	}
	if (contains_any(normalized, {"download", "fetch", "transfer", "connect", "receive", "pull"})) {
		return ProgressPhase::TRANSFER;
	}
	if (contains_any(normalized, {"extract", "unpack", "archive", "decompress", "inflate", "untar", "unzip"})) {
		return ProgressPhase::POST_TRANSFER;
	}
	if (contains_any(normalized, {"install", "link", "symlink", "refresh", "registry", "plugin", "verify", "write"})) {
		return ProgressPhase::POST_TRANSFER;
	}
	return ProgressPhase::INDETERMINATE;
}

bool show_transfer_metrics(const ProgressPhase phase) {
	return phase == ProgressPhase::TRANSFER;
}

DisplayProgressMetrics metrics_for_display_phase(const DisplayProgressMetrics& metrics, const ProgressPhase phase) {
	if (show_transfer_metrics(phase)) {
		return canonicalize_progress_metrics(metrics);
	}
	DisplayProgressMetrics trimmed = canonicalize_progress_metrics(metrics);
	trimmed.currentBytes.reset();
	trimmed.totalBytes.reset();
	trimmed.bytesPerSecond.reset();
	return trimmed;
}

std::string format_progress_tail(const DisplayProgressMetrics& metrics, const ProgressPhase phase) {
	const DisplayProgressMetrics displayMetrics = metrics_for_display_phase(metrics, phase);
	std::string summary = format_progress_summary(displayMetrics);
	if (!summary.empty() && show_transfer_metrics(phase) && metrics.bytesPerSecond.has_value()) {
		return summary;
	}
	if (!summary.empty()) {
		return summary;
	}
	if (displayMetrics.percent.has_value()) {
		return std::to_string(displayMetrics.percent.value()) + "%";
	}
	return {};
}

}  // namespace reqpack::terminal
