#include "output/terminal/speed_ema.h"

#include <optional>

namespace reqpack::terminal {

void SpeedEma::reset() {
	smoothed_.reset();
}

void SpeedEma::observe(const std::uint64_t bytesPerSecond) {
	const double sample = static_cast<double>(bytesPerSecond);
	if (!smoothed_.has_value()) {
		smoothed_ = sample;
		return;
	}
	// speed_new = 0.2 * current + 0.8 * old
	smoothed_ = 0.2 * sample + 0.8 * smoothed_.value();
}

std::optional<std::uint64_t> SpeedEma::value() const {
	if (!smoothed_.has_value()) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(smoothed_.value());
}

}  // namespace reqpack::terminal
