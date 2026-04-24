#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <vector>

enum class OutputAction {
	LOG,
	STDOUT,
	FLUSH,
	STOP
};

struct OutputContext {
	spdlog::level::level_enum level{spdlog::level::info};
	std::string message{};
	std::string source{};
	std::string scope{};
};

struct OutputEvent {
	OutputAction action{OutputAction::LOG};
	OutputContext context{};
};

class Logger {
	std::shared_ptr<spdlog::logger> logger;
	std::vector<spdlog::sink_ptr> sinks;
	std::thread worker;
	std::deque<OutputEvent> queue;
	std::mutex queueMutex;
	std::condition_variable queueCondition;
	bool stopRequested{false};
	std::string pattern{"%^[%T] [%l] %v%$"};

	Logger(const Logger&) = delete;
	Logger& operator=(const Logger&) = delete;

	void startWorker();
	void enqueue(OutputEvent event);
	void processLoop();
	void processEvent(const OutputEvent& event);
	static std::string formatMessage(const OutputContext& context);

public:
	Logger();
	~Logger();

	static Logger& instance();

	void setLevel(const std::string& level);
	void setLevel(spdlog::level::level_enum level);
	void setPattern(const std::string& pattern);
	
	void setFileSink(const std::string& filename);
	void setBacktrace(bool enable, size_t max_size = 10);
	void emit(OutputAction action, const OutputContext& context = {});
	void stdout(const std::string& message, const std::string& source = {}, const std::string& scope = {});
	void flush();

	void crit(const std::string& message);
	void err(const std::string& message);
	void warn(const std::string& message);
	void info(const std::string& message);
	void debug(const std::string& message);
	void trace(const std::string& message);
};
