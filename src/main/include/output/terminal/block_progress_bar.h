#pragma once

#include <string>

namespace reqpack::terminal {

/// Unicode block bar with eighth-step fill (width × 8 resolution).
std::string render_fractional_block_bar(double ratio, int width);

}  // namespace reqpack::terminal
