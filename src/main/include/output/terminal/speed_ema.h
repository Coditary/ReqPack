#pragma once

#include <cstdint>
#include <optional>

namespace reqpack::terminal {

/// Exponential moving average for transfer rates (smoother MiB/s display).
class SpeedEma {
public:
	void reset();
	void observe(std::uint64_t bytesPerSecond);
	std::optional<std::uint64_t> value() const;

private:
	std::optional<double> smoothed_;
};

}  // namespace reqpack::terminal
