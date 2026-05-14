#pragma once

#include "core/config/configuration.h"

class Logger;

int run_self_update(const ReqPackConfig& config, Logger& logger);
