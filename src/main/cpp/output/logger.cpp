#include "output/logger.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

Logger::Logger() {
    // Standardmäßig farbige Konsolenausgabe erstellen
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(console_sink);

    // Logger instanziieren (Name "Global")
    logger = std::make_shared<spdlog::logger>("Global", sinks.begin(), sinks.end());
    
    // Standard-Einstellungen
    logger->set_level(spdlog::level::info);
    logger->set_pattern("%^[%T] [%l] %v%$");
    
    // Registrierung im spdlog-Pool (optional, erlaubt Zugriff via spdlog::get("Global"))
    spdlog::register_logger(logger);
}

Logger::~Logger() {
    spdlog::drop(logger->name());
}

void Logger::setLevel(const std::string& level) {
    // Umwandlung von String zu spdlog-Enum
    spdlog::level::level_enum lvl = spdlog::level::from_str(level);
    logger->set_level(lvl);
}

void Logger::setLevel(spdlog::level::level_enum level) {
    logger->set_level(level);
}

void Logger::setPattern(const std::string& pattern) {
    logger->set_pattern(pattern);
}

void Logger::setFileSink(const std::string& filename) {
    // Neuen Datei-Sink erstellen
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
    
    // Sink zur Liste und zum Logger hinzufügen
    sinks.push_back(file_sink);
    
    // Da spdlog-Logger-Sinks zur Laufzeit nicht einfach via Vektor getauscht werden,
    // weisen wir die neue Sink-Liste dem Logger direkt zu
    logger->sinks() = sinks;
}

void Logger::setBacktrace(bool enable, size_t max_size) {
    if (enable) {
        logger->enable_backtrace(max_size);
    } else {
        logger->disable_backtrace();
    }
}

// Logging-Methoden
void Logger::crit(const std::string& message) {
    logger->critical(message);
    // Backtrace im Falle eines Critical-Errors ausspucken, falls aktiviert
    logger->dump_backtrace(); 
}

void Logger::err(const std::string& message)   { logger->error(message); }
void Logger::warn(const std::string& message)  { logger->warn(message); }
void Logger::info(const std::string& message)  { logger->info(message); }
void Logger::debug(const std::string& message) { logger->debug(message); }
void Logger::trace(const std::string& message) { logger->trace(message); }
