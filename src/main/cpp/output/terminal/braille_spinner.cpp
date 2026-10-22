#include "output/terminal/braille_spinner.h"

namespace reqpack::terminal {

std::string_view braille_spinner_frame(const std::size_t tick) {
	return kBrailleSpinnerFrames[tick % kBrailleSpinnerFrameCount];
}

}  // namespace reqpack::terminal
