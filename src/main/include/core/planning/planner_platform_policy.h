#pragma once

#include <string>

namespace planner_platform {

inline bool isNixInstallSystem(const std::string& system) {
	return system == "nix";
}

#if defined(_WIN32)
inline constexpr bool reorderNonNixBeforeNix = true;
#else
inline constexpr bool reorderNonNixBeforeNix = false;
#endif

// On Windows, ignore edges nix -> non-nix for scheduling so Non-nix can run first
// without creating a cycle with the Non-nix-before-nix barrier.
inline bool shouldIgnoreScheduleEdge(const std::string& sourceSystem, const std::string& targetSystem) {
#if defined(_WIN32)
	return isNixInstallSystem(sourceSystem) && !isNixInstallSystem(targetSystem);
#else
	(void)sourceSystem;
	(void)targetSystem;
	return false;
#endif
}

inline bool needsNonNixBeforeNixBarrier() {
#if defined(_WIN32)
	return true;
#else
	return false;
#endif
}

inline bool softSkipNixInstalls() {
#if defined(_WIN32)
	return true;
#else
	return false;
#endif
}

}  // namespace planner_platform
