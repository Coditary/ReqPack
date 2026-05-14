#include "output/logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>

namespace {

std::once_flag LOGGER_INSTANCE_FLAG;
Logger* LOGGER_INSTANCE = nullptr;

}  // namespace

Logger::Logger() {
    auto consoleSinkHandle = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink = consoleSinkHandle;
    sinks.push_back(consoleSinkHandle);

    logger = std::make_shared<spdlog::logger>("Global", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    logger->set_pattern(pattern);
    updateConsoleSinkLevel();

    spdlog::register_logger(logger);
    startWorker();
}

Logger::~Logger() {
    emit(OutputAction::STOP);
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
        processLoop();
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

        processEvent(event);
        {
            std::lock_guard<std::mutex> lock(processedMutex);
            processedEventId = std::max(processedEventId, event.id);
        }
        processedCondition.notify_all();
        if (event.action == OutputAction::STOP) {
            break;
        }
    }
}
