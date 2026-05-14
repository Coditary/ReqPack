#pragma once

#include "output/command_output.h"

#include <memory>
#include <ostream>

namespace command_output_internal {

std::unique_ptr<IDisplay> make_plain_stream_display(std::ostream& stream);

}  // namespace command_output_internal
