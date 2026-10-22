#pragma once

#include <cstddef>
#include <string_view>

namespace reqpack::terminal {

inline constexpr std::string_view kBrailleSpinnerFrames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};

inline constexpr std::size_t kBrailleSpinnerFrameCount =
    sizeof(kBrailleSpinnerFrames) / sizeof(kBrailleSpinnerFrames[0]);

/// Advance frame index (e.g. every ~80ms).
std::string_view braille_spinner_frame(std::size_t tick);

}  // namespace reqpack::terminal
