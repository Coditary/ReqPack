#pragma once

#include <chrono>

namespace reqpack::terminal {

/// Default ~30 FPS; use 16ms for ~60 FPS.
inline constexpr auto kDefaultRenderInterval = std::chrono::milliseconds(33);

/// Returns true when enough time passed since `last`, or when `force` is set.
template <typename Clock = std::chrono::steady_clock>
bool should_render(const typename Clock::time_point& now,
                   typename Clock::time_point& last,
                   std::chrono::milliseconds interval = kDefaultRenderInterval,
                   bool force = false) {
	if (force) {
		last = now;
		return true;
	}
	if (last == typename Clock::time_point{}) {
		last = now;
		return true;
	}
	if (now - last >= interval) {
		last = now;
		return true;
	}
	return false;
}

}  // namespace reqpack::terminal
