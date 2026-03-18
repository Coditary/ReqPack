#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <memory>
#include <vector>

class Logger {
	std::shared_ptr<spdlog::logger> logger;
	std::vector<spdlog::sink_ptr> sinks;

public:
	Logger();
	~Logger();

	void setLevel(const std::string& level);
	void setLevel(spdlog::level::level_enum level);
	void setPattern(const std::string& pattern);
	
	void setFileSink(const std::string& filename);
	void setBacktrace(bool enable, size_t max_size = 10);

	void crit(const std::string& message);
	void err(const std::string& message);
	void warn(const std::string& message);
	void info(const std::string& message);
	void debug(const std::string& message);
	void trace(const std::string& message);

};
