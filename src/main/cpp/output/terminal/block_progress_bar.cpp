#include "output/terminal/block_progress_bar.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace reqpack::terminal {

namespace {

constexpr const char* kEmptyBlock = "░";
constexpr const char* kFullBlock = "█";

}  // namespace

std::string render_fractional_block_bar(const double ratio, const int width) {
	if (width <= 0) {
		return {};
	}
	const double clamped = std::clamp(ratio, 0.0, 1.0);
	const int filledCells = std::clamp(static_cast<int>(std::llround(clamped * static_cast<double>(width))), 0, width);

	std::string bar;
	bar.reserve(static_cast<size_t>(width) * 3);
	for (int i = 0; i < width; ++i) {
		bar += (i < filledCells) ? kFullBlock : kEmptyBlock;
	}
	return bar;
}

}  // namespace reqpack::terminal
