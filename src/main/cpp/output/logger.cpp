#include "output/logger.h"
#include "output/logger_core.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>

namespace {

std::once_flag LOGGER_INSTANCE_FLAG;
Logger* LOGGER_INSTANCE = nullptr;

}  // namespace

Logger::Logger() {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(console_sink);

    logger = std::make_shared<spdlog::logger>("Global", sinks.begin(), sinks.end());
    
    logger->set_level(spdlog::level::info);
    logger->set_pattern(pattern);
    
    spdlog::register_logger(logger);
	this->startWorker();
}

Logger::~Logger() {
	this->emit(OutputAction::STOP);
	if (worker.joinable()) {
		worker.join();
	}
    spdlog::drop(logger->name());
}

Logger& Logger::instance() {
	std::call_once(LOGGER_INSTANCE_FLAG, []() {
		LOGGER_INSTANCE = new Logger();
	});

	return *LOGGER_INSTANCE;
}

void Logger::startWorker() {
	worker = std::thread([this]() {
		this->processLoop();
	});
}

void Logger::enqueue(OutputEvent event) {
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		queue.push_back(std::move(event));
	}
	queueCondition.notify_one();
}

void Logger::processLoop() {
	for (;;) {
		OutputEvent event;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueCondition.wait(lock, [this]() {
				return !queue.empty();
			});

			event = std::move(queue.front());
			queue.pop_front();
		}

		this->processEvent(event);
		if (event.action == OutputAction::STOP) {
			break;
		}
	}
}

void Logger::processEvent(const OutputEvent& event) {
	switch (event.action) {
		case OutputAction::LOG:
			logger->log(event.context.level, logger_render_output_event(event));
			if (event.context.level == spdlog::level::critical) {
				logger->dump_backtrace();
			}
			break;
		case OutputAction::STDOUT:
			std::cout << logger_render_output_event(event);
			if (logger_stdout_needs_trailing_newline(event.context)) {
				std::cout << '\n';
			}
			std::cout.flush();
			break;
		case OutputAction::PLUGIN_STATUS:
			std::cout << logger_render_output_event(event);
			std::cout << '\n';
			std::cout.flush();
			break;
		case OutputAction::PLUGIN_PROGRESS:
			std::cout << logger_render_output_event(event);
			std::cout << '\n';
			std::cout.flush();
			break;
		case OutputAction::PLUGIN_EVENT:
			std::cout << logger_render_output_event(event);
			std::cout << '\n';
			std::cout.flush();
			break;
		case OutputAction::PLUGIN_ARTIFACT:
			std::cout << logger_render_output_event(event);
			std::cout << '\n';
			std::cout.flush();
			break;
		case OutputAction::FLUSH:
			for (const auto& sink : sinks) {
				sink->flush();
			}
			std::cout.flush();
			break;
		case OutputAction::STOP:
			for (const auto& sink : sinks) {
				sink->flush();
			}
			std::cout.flush();
			break;
	}
}

std::string Logger::formatMessage(const OutputContext& context) {
	return logger_format_message(context);
}

void Logger::setLevel(const std::string& level) {
    spdlog::level::level_enum lvl = spdlog::level::from_str(level);
    logger->set_level(lvl);
}

void Logger::setLevel(spdlog::level::level_enum level) {
    logger->set_level(level);
}

void Logger::setPattern(const std::string& pattern) {
    this->pattern = pattern;
    logger->set_pattern(pattern);
}

void Logger::setFileSink(const std::string& filename) {
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
    
    sinks.push_back(file_sink);
    
    logger->sinks() = sinks;
}

void Logger::setBacktrace(bool enable, size_t max_size) {
    if (enable) {
        logger->enable_backtrace(max_size);
    } else {
        logger->disable_backtrace();
    }
}

void Logger::emit(OutputAction action, const OutputContext& context) {
	this->enqueue(OutputEvent{.action = action, .context = context});
}

void Logger::stdout(const std::string& message, const std::string& source, const std::string& scope) {
	this->emit(OutputAction::STDOUT, OutputContext{.level = spdlog::level::info, .message = message, .source = source, .scope = scope});
}

void Logger::flush() {
	this->emit(OutputAction::FLUSH);
}

void Logger::crit(const std::string& message) {
    this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::critical, .message = message});
}

void Logger::err(const std::string& message)   { this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::err, .message = message}); }
void Logger::warn(const std::string& message)  { this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::warn, .message = message}); }
void Logger::info(const std::string& message)  { this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::info, .message = message}); }
void Logger::debug(const std::string& message) { this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::debug, .message = message}); }
void Logger::trace(const std::string& message) { this->emit(OutputAction::LOG, OutputContext{.level = spdlog::level::trace, .message = message}); }
