#include "output/logger.h"
#include "output/logger_core.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string normalize_category(const std::string& category) {
    return to_lower_copy(category);
}

std::string inferred_category(const OutputEvent& event) {
    if (!event.context.category.empty()) {
        return normalize_category(event.context.category);
    }
    switch (event.action) {
        case OutputAction::PLUGIN_STATUS:
        case OutputAction::PLUGIN_PROGRESS:
        case OutputAction::PLUGIN_EVENT:
        case OutputAction::PLUGIN_ARTIFACT:
            return "plugin";
        case OutputAction::DISPLAY_SESSION_BEGIN:
        case OutputAction::DISPLAY_SESSION_END:
        case OutputAction::DISPLAY_ITEM_BEGIN:
        case OutputAction::DISPLAY_ITEM_PROGRESS:
        case OutputAction::DISPLAY_ITEM_STEP:
        case OutputAction::DISPLAY_ITEM_SUCCESS:
        case OutputAction::DISPLAY_ITEM_FAILURE:
        case OutputAction::DISPLAY_MESSAGE:
        case OutputAction::DISPLAY_TABLE_HEADER:
        case OutputAction::DISPLAY_TABLE_ROW:
        case OutputAction::DISPLAY_TABLE_END:
            return "display";
        case OutputAction::STDOUT:
            return "stdout";
        case OutputAction::LOG:
        case OutputAction::DIAGNOSTIC:
            return "general";
        case OutputAction::FLUSH:
        case OutputAction::STOP:
            return {};
    }
    return "general";
}

std::string utc_timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&currentTime, &tm);
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

}  // namespace

std::string Logger::formatMessage(const OutputContext& context) {
    return logger_format_message(context);
}

void Logger::refreshSinks() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    std::vector<spdlog::sink_ptr> refreshed;
    if (consoleSink) {
        refreshed.push_back(consoleSink);
    }
    if (textFileSink) {
        refreshed.push_back(textFileSink);
    }
    sinks = std::move(refreshed);
    logger->sinks() = sinks;
}

bool Logger::isCategoryEnabled(const OutputEvent& event) const {
    const std::string category = inferred_category(event);
    std::lock_guard<std::mutex> lock(settingsMutex);
    if (enabledCategories.empty() || category.empty()) {
        return true;
    }
    return std::find(enabledCategories.begin(), enabledCategories.end(), category) != enabledCategories.end();
}

bool Logger::shouldCaptureDisplayEvents() const {
    std::lock_guard<std::mutex> lock(settingsMutex);
    return captureDisplayEvents;
}

void Logger::writeStructuredEvent(const OutputEvent& event) {
    OutputEvent enriched = event;
    enriched.context.category = inferred_category(event);
    if (enriched.context.message.empty()) {
        enriched.context.message = !enriched.context.payload.empty() ? enriched.context.payload : logger_render_output_event(event);
    }
    std::string line = logger_render_structured_event_json(enriched);
    if (line.empty()) {
        return;
    }
    const std::string timestamp = utc_timestamp_now();
    const std::string needle = "\"timestamp\":\"\"";
    const std::size_t offset = line.find(needle);
    if (offset != std::string::npos) {
        line.replace(offset, needle.size(), "\"timestamp\":\"" + logger_escape_json(timestamp) + "\"");
    }
    std::lock_guard<std::mutex> lock(settingsMutex);
    if (!structuredFileStream.is_open()) {
        return;
    }
    structuredFileStream << line << '\n';
    structuredFileStream.flush();
}

void Logger::updateConsoleSinkLevel() {
    if (!consoleSink) {
        return;
    }
    if (!consoleOutputEnabled.load(std::memory_order_acquire)) {
        consoleSink->set_level(spdlog::level::off);
        return;
    }
    if (display.load(std::memory_order_acquire) != nullptr) {
        consoleSink->set_level(spdlog::level::off);
        return;
    }
    consoleSink->set_level(spdlog::level::trace);
}

void Logger::setDisplay(IDisplay* activeDisplay) {
    display.store(activeDisplay, std::memory_order_release);
    updateConsoleSinkLevel();
}

void Logger::setConsoleOutput(bool enable) {
    consoleOutputEnabled.store(enable, std::memory_order_release);
    updateConsoleSinkLevel();
}

bool Logger::isConsoleOutputEnabled() const {
    return consoleOutputEnabled.load(std::memory_order_acquire);
}

void Logger::setLevel(const std::string& level) {
    logger->set_level(spdlog::level::from_str(level));
}

void Logger::setLevel(spdlog::level::level_enum level) {
    logger->set_level(level);
}

void Logger::setPattern(const std::string& updatedPattern) {
    pattern = updatedPattern;
    logger->set_pattern(updatedPattern);
}

void Logger::setFileSink(const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        textFileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
    }
    refreshSinks();
}

void Logger::disableFileSink() {
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        textFileSink.reset();
    }
    refreshSinks();
}

void Logger::setStructuredFileSink(const std::string& filename) {
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        structuredFileStream.close();
        structuredFileStream.clear();
        const std::filesystem::path path(filename);
        if (path.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
        }
        structuredFileStream.open(filename, std::ios::out | std::ios::app);
        structuredFilePath = filename;
        if (!structuredFileStream.is_open()) {
            structuredFilePath.clear();
        }
    }
}

void Logger::disableStructuredFileSink() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    structuredFileStream.close();
    structuredFileStream.clear();
    structuredFilePath.clear();
}

void Logger::setCaptureDisplayEvents(bool enable) {
    std::lock_guard<std::mutex> lock(settingsMutex);
    captureDisplayEvents = enable;
}

void Logger::setEnabledCategories(const std::vector<std::string>& categories) {
    std::vector<std::string> normalized = categories;
    for (std::string& category : normalized) {
        category = normalize_category(category);
    }
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](const std::string& category) {
        return category.empty();
    }), normalized.end());
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    std::lock_guard<std::mutex> lock(settingsMutex);
    enabledCategories = std::move(normalized);
}

void Logger::setBacktrace(bool enable, size_t maxSize) {
    if (enable) {
        logger->enable_backtrace(maxSize);
    } else {
        logger->disable_backtrace();
    }
}
