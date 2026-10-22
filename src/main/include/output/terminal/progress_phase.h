#pragma once

#include "output/progress_metrics.h"

#include <string>
#include <string_view>

namespace reqpack::terminal {

enum class ProgressPhase {
	INDETERMINATE,
	TRANSFER,
	POST_TRANSFER,
};

ProgressPhase classify_progress_step(std::string_view step);

bool show_transfer_metrics(ProgressPhase phase);

/// Metrics with byte/speed fields stripped when not in a transfer phase.
DisplayProgressMetrics metrics_for_display_phase(const DisplayProgressMetrics& metrics, ProgressPhase phase);

std::string format_progress_tail(const DisplayProgressMetrics& metrics, ProgressPhase phase);

}  // namespace reqpack::terminal
