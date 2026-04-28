#include "output/display_factory.h"

#include "output/color_display.h"
#include "output/plain_display.h"

#include <memory>

std::unique_ptr<IDisplay> create_display(const DisplayConfig& config) {
	switch (config.renderer) {
		case DisplayRenderer::COLOR:
			return std::make_unique<ColorDisplay>(config.colors);
		case DisplayRenderer::PLAIN:
		default:
			return std::make_unique<PlainDisplay>();
	}
}
