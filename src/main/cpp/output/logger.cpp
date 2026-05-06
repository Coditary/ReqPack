#include "output/logger.h"
#include "output/logger_core.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::once_flag LOGGER_INSTANCE_FLAG;
Logger*        LOGGER_INSTANCE = nullptr;

/// Split a pipe-delimited string into a vector of strings.
std::vector<std::string> splitPipe(const std::string& s) {
	std::vector<std::string> out;
	std::istringstream       ss(s);
	std::string              token;
	while (std::getline(ss, token, '|')) {
		out.push_back(std::move(token));
	}
	if (!s.empty() && s.back() == '|') {
		out.emplace_back();
	}
	return out;
}

/// Join a vector of strings with a pipe delimiter.
std::string joinPipe(const std::vector<std::string>& v) {
	std::string out;
	for (size_t i = 0; i < v.size(); ++i) {
		if (i) out += '|';
		out += v[i];
	}
	return out;
}

std::string to_lower_copy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

std::string normalize_category(const std::string& category) {
	return to_lower_copy(category);
}

bool is_display_action(OutputAction action) {
	switch (action) {
		case OutputAction::DISPLAY_SESSION_BEGIN:
		case OutputAction::DISPLAY_SESSION_END:
		case OutputAction::DISPLAY_ITEM_BEGIN:
		case OutputAction::DISPLAY_ITEM_STEP:
		case OutputAction::DISPLAY_ITEM_SUCCESS:
		case OutputAction::DISPLAY_ITEM_FAILURE:
		case OutputAction::DISPLAY_MESSAGE:
		case OutputAction::DISPLAY_TABLE_HEADER:
		case OutputAction::DISPLAY_TABLE_ROW:
		case OutputAction::DISPLAY_TABLE_END:
			return true;
		default:
			return false;
	}
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

void emit_message_lines(IDisplay* display, const std::string& text, const std::string& source) {
	if (display == nullptr) {
		return;
	}
	std::istringstream stream(text);
	std::string line;
	bool emitted = false;
	while (std::getline(stream, line)) {
		display->onMessage(line, source);
		emitted = true;
	}
	if (!emitted && !text.empty()) {
		display->onMessage(text, source);
	}
}

bool is_item_scoped_plugin_source(const std::string& source) {
	return source != "plugin" && source.find(':') != std::string::npos;
}

std::string plugin_display_source(const OutputContext& context) {
	if (is_item_scoped_plugin_source(context.source)) {
		return context.source;
	}
	if (!context.scope.empty()) {
		return context.scope;
	}
	return context.source;
}

DisplayProgressMetrics progress_metrics_from_context(const OutputContext& context) {
	return canonicalize_progress_metrics(DisplayProgressMetrics{
		.percent = context.progressPercent,
		.currentBytes = context.currentBytes,
		.totalBytes = context.totalBytes,
		.bytesPerSecond = context.bytesPerSecond,
	});
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Logger::Logger() {
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	consoleSink = console_sink;
	sinks.push_back(console_sink);

	logger = std::make_shared<spdlog::logger>("Global", sinks.begin(), sinks.end());

	logger->set_level(spdlog::level::info);
	logger->set_pattern(pattern);
    updateConsoleSinkLevel();

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

// ─────────────────────────────────────────────────────────────────────────────
// Worker
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Event processing
// ─────────────────────────────────────────────────────────────────────────────

void Logger::routeToDisplay(const OutputEvent& event) {
	IDisplay* d = display.load(std::memory_order_acquire);
	if (d == nullptr) return;

	const OutputContext& ctx = event.context;
	if (event.action == OutputAction::DIAGNOSTIC) {
		emit_message_lines(d, logger_diagnostic_text(ctx, false), ctx.source);
		return;
	}
	const bool itemScoped = is_item_scoped_plugin_source(ctx.source);
	const std::string displaySource = plugin_display_source(ctx);

	switch (event.action) {
		// ── Plugin callbacks → display ────────────────────────────────────────
		case OutputAction::PLUGIN_PROGRESS: {
			const DisplayProgressMetrics metrics = progress_metrics_from_context(ctx);
			if (itemScoped) {
				d->onItemProgress(ctx.source, metrics);
			} else {
				const std::string summary = format_progress_summary(metrics);
				d->onMessage(summary.empty() ? "progress" : "progress " + summary, displaySource);
			}
			break;
		}

		case OutputAction::PLUGIN_STATUS:
			d->onMessage("status " + std::to_string(ctx.statusCode), displaySource);
			break;

		case OutputAction::PLUGIN_EVENT:
			if (ctx.eventName == "begin_step") {
				if (itemScoped) {
					d->onItemStep(ctx.source, ctx.payload);
				} else {
					d->onMessage(ctx.payload, displaySource);
				}
				break;
			}
			if (ctx.eventName == "success") {
				if (itemScoped) {
					d->onItemSuccess(ctx.source);
				} else {
					d->onMessage("done", displaySource);
				}
				break;
			}
			if (ctx.eventName == "failed") {
				if (itemScoped) {
					d->onItemFailure(ctx.source, ctx.payload);
				} else {
					d->onMessage(ctx.payload.empty() ? "failed" : "failed: " + ctx.payload, displaySource);
				}
				break;
			}
			if (ctx.eventName == "installed" || ctx.eventName == "deleted" || ctx.eventName == "updated" ||
			    ctx.eventName == "listed" || ctx.eventName == "searched" || ctx.eventName == "informed" ||
			    ctx.eventName == "outdated") {
				break;
			}
			d->onMessage(ctx.eventName + ": " + ctx.payload, displaySource);
			break;

		case OutputAction::PLUGIN_ARTIFACT:
			d->onMessage("artifact: " + ctx.payload, displaySource);
			break;

		// ── Session lifecycle ─────────────────────────────────────────────────
		case OutputAction::DISPLAY_SESSION_BEGIN: {
			auto mode  = static_cast<DisplayMode>(ctx.displayMode);
			auto items = splitPipe(ctx.payload);
			d->onSessionBegin(mode, items);
			break;
		}

		case OutputAction::DISPLAY_SESSION_END: {
			// payload = "<succeeded>:<failed>"
			bool ok        = (ctx.statusCode == 0);
			int  succeeded = 0;
			int  skipped   = 0;
			int  failed    = 0;
			auto firstSep = ctx.payload.find(':');
			auto secondSep = firstSep == std::string::npos ? std::string::npos : ctx.payload.find(':', firstSep + 1);
			if (firstSep != std::string::npos && secondSep != std::string::npos) {
				succeeded = std::stoi(ctx.payload.substr(0, firstSep));
				skipped   = std::stoi(ctx.payload.substr(firstSep + 1, secondSep - firstSep - 1));
				failed    = std::stoi(ctx.payload.substr(secondSep + 1));
			}
			d->onSessionEnd(ok, succeeded, skipped, failed);
			break;
		}

		// ── Item lifecycle ────────────────────────────────────────────────────
		case OutputAction::DISPLAY_ITEM_BEGIN:
			d->onItemBegin(ctx.source, ctx.message.empty() ? ctx.source : ctx.message);
			break;

		case OutputAction::DISPLAY_ITEM_STEP:
			d->onItemStep(ctx.source, ctx.message);
			break;

		case OutputAction::DISPLAY_ITEM_SUCCESS:
			d->onItemSuccess(ctx.source);
			break;

		case OutputAction::DISPLAY_ITEM_FAILURE:
			d->onItemFailure(ctx.source, ctx.message);
			break;

		case OutputAction::DISPLAY_MESSAGE:
			emit_message_lines(d, ctx.message, ctx.source);
			break;

		// ── Table output ──────────────────────────────────────────────────────
		case OutputAction::DISPLAY_TABLE_HEADER:
			d->onTableBegin(splitPipe(ctx.payload));
			break;

		case OutputAction::DISPLAY_TABLE_ROW:
			d->onTableRow(splitPipe(ctx.payload));
			break;

		case OutputAction::DISPLAY_TABLE_END:
			d->onTableEnd();
			break;

		// ── STDOUT → display message ──────────────────────────────────────────
		case OutputAction::STDOUT:
			d->onMessage(logger_format_message(ctx));
			break;

		default:
			break;
	}
}

void Logger::processEvent(const OutputEvent& event) {
	IDisplay* d = display.load(std::memory_order_acquire);
	const bool categoryEnabled = isCategoryEnabled(event);
	const bool captureDisplay = shouldCaptureDisplayEvents();
	const bool persistStructured = categoryEnabled && (!is_display_action(event.action) || captureDisplay);
	if (persistStructured) {
		writeStructuredEvent(event);
	}

	switch (event.action) {
		case OutputAction::LOG:
			if (categoryEnabled) {
				logger->log(event.context.level, logger_render_output_event(event));
			}
			if (categoryEnabled && d != nullptr &&
			    (event.context.level >= spdlog::level::warn || consoleOutputEnabled.load(std::memory_order_acquire))) {
				d->onMessage(logger_format_message(event.context));
			}
			if (event.context.level == spdlog::level::critical) {
				logger->dump_backtrace();
			}
			break;

		case OutputAction::DIAGNOSTIC:
			if (categoryEnabled) {
				logger->log(event.context.level, logger_render_output_event(event));
			}
			if (d != nullptr && event.context.mirrorToDisplay) {
				routeToDisplay(event);
			}
			break;

		case OutputAction::STDOUT:
			if (d != nullptr) {
				routeToDisplay(event);
			} else {
				std::cout << logger_render_output_event(event);
				if (logger_stdout_needs_trailing_newline(event.context)) {
					std::cout << '\n';
				}
				std::cout.flush();
			}
			break;

		case OutputAction::PLUGIN_STATUS:
		case OutputAction::PLUGIN_PROGRESS:
		case OutputAction::PLUGIN_EVENT:
		case OutputAction::PLUGIN_ARTIFACT:
			if (d != nullptr) {
				routeToDisplay(event);
			} else {
				std::cout << logger_render_output_event(event) << '\n';
				std::cout.flush();
			}
			break;

		case OutputAction::DISPLAY_SESSION_BEGIN:
		case OutputAction::DISPLAY_SESSION_END:
		case OutputAction::DISPLAY_ITEM_BEGIN:
		case OutputAction::DISPLAY_ITEM_STEP:
		case OutputAction::DISPLAY_ITEM_SUCCESS:
		case OutputAction::DISPLAY_ITEM_FAILURE:
		case OutputAction::DISPLAY_MESSAGE:
		case OutputAction::DISPLAY_TABLE_HEADER:
		case OutputAction::DISPLAY_TABLE_ROW:
		case OutputAction::DISPLAY_TABLE_END:
			if (d != nullptr) {
				routeToDisplay(event);
			}
			break;

		case OutputAction::FLUSH:
		case OutputAction::STOP:
			if (d != nullptr) {
				d->flush();
			}
			for (const auto& sink : sinks) {
				sink->flush();
			}
			{
				std::lock_guard<std::mutex> lock(settingsMutex);
				if (structuredFileStream.is_open()) {
					structuredFileStream.flush();
				}
			}
			std::cout.flush();
			break;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

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

void Logger::setDisplay(IDisplay* d) {
	display.store(d, std::memory_order_release);
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

void Logger::setBacktrace(bool enable, size_t max_size) {
	if (enable) {
		logger->enable_backtrace(max_size);
	} else {
		logger->disable_backtrace();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Core emit
// ─────────────────────────────────────────────────────────────────────────────

std::uint64_t Logger::emit(OutputAction action, const OutputContext& context) {
	std::uint64_t eventId = 0;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		eventId = ++nextEventId;
		queue.push_back(OutputEvent{.id = eventId, .action = action, .context = context});
	}
	queueCondition.notify_one();
	return eventId;
}

std::uint64_t Logger::emitDiagnostic(const DiagnosticMessage& diagnostic, bool mirrorToDisplay) {
	return this->emit(OutputAction::DIAGNOSTIC,
	                  OutputContext{
		                  .level = diagnostic.severity,
		                  .message = diagnostic.summary,
		                  .category = diagnostic.category,
		                  .cause = diagnostic.cause,
		                  .recommendation = diagnostic.recommendation,
		                  .details = diagnostic.details,
		                  .source = diagnostic.source,
		                  .scope = diagnostic.scope,
		                  .contextFields = diagnostic.context,
		                  .mirrorToDisplay = mirrorToDisplay,
	                  });
}

void Logger::stdout(const std::string& message,
                     const std::string& source,
                     const std::string& scope) {
	this->emit(OutputAction::STDOUT,
	           OutputContext{.level   = spdlog::level::info,
	                         .message = message,
	                         .source  = source,
	                         .scope   = scope});
}

void Logger::flush() {
	this->emit(OutputAction::FLUSH);
}

void Logger::flushSync() {
	const std::uint64_t flushEventId = this->emit(OutputAction::FLUSH);
	std::unique_lock<std::mutex> lock(processedMutex);
	processedCondition.wait(lock, [&]() {
		return processedEventId >= flushEventId;
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Spdlog convenience wrappers
// ─────────────────────────────────────────────────────────────────────────────

void Logger::crit(const std::string& message) {
	this->emit(OutputAction::LOG,
	           OutputContext{.level = spdlog::level::critical, .message = message});
}
void Logger::err  (const std::string& m) { this->emit(OutputAction::LOG, {.level = spdlog::level::err,   .message = m}); }
void Logger::warn (const std::string& m) { this->emit(OutputAction::LOG, {.level = spdlog::level::warn,  .message = m}); }
void Logger::info (const std::string& m) { this->emit(OutputAction::LOG, {.level = spdlog::level::info,  .message = m}); }
void Logger::debug(const std::string& m) { this->emit(OutputAction::LOG, {.level = spdlog::level::debug, .message = m}); }
void Logger::trace(const std::string& m) { this->emit(OutputAction::LOG, {.level = spdlog::level::trace, .message = m}); }

void Logger::diagnostic(const DiagnosticMessage& diagnostic, bool mirrorToDisplay) {
	(void)this->emitDiagnostic(diagnostic, mirrorToDisplay);
}

// ─────────────────────────────────────────────────────────────────────────────
// Display session helpers
// ─────────────────────────────────────────────────────────────────────────────

void Logger::displaySessionBegin(DisplayMode                    mode,
                                   const std::vector<std::string>& items) {
	this->emit(OutputAction::DISPLAY_SESSION_BEGIN,
	           OutputContext{.payload     = joinPipe(items),
	                         .displayMode = static_cast<int>(mode)});
}

void Logger::displaySessionEnd(bool success, int succeeded, int skipped, int failed) {
	this->emit(OutputAction::DISPLAY_SESSION_END,
	           OutputContext{.statusCode = success ? 0 : 1,
	                         .payload   = std::to_string(succeeded) + ":" +
	                                      std::to_string(skipped) + ":" +
	                                      std::to_string(failed)});
}

// ─────────────────────────────────────────────────────────────────────────────
// Display item helpers
// ─────────────────────────────────────────────────────────────────────────────

void Logger::displayItemBegin(const std::string& itemId,
                               const std::string& label) {
	this->emit(OutputAction::DISPLAY_ITEM_BEGIN,
	           OutputContext{.message = label, .source = itemId});
}

void Logger::displayItemStep(const std::string& itemId,
                              const std::string& step) {
	this->emit(OutputAction::DISPLAY_ITEM_STEP,
	           OutputContext{.message = step, .source = itemId});
}

void Logger::displayItemSuccess(const std::string& itemId) {
	this->emit(OutputAction::DISPLAY_ITEM_SUCCESS,
	           OutputContext{.source = itemId});
}

void Logger::displayItemFailure(const std::string& itemId,
                                 const std::string& reason) {
	this->emit(OutputAction::DISPLAY_ITEM_FAILURE,
	           OutputContext{.message = reason, .source = itemId});
}

void Logger::displayItemFailure(const std::string& itemId,
	                             const DiagnosticMessage& diagnostic) {
	this->displayItemFailure(itemId, diagnostic.summary);
	this->displayDiagnostic(diagnostic, itemId, false);
}

void Logger::displayDiagnostic(const DiagnosticMessage& diagnostic,
	                          const std::string& sourceOverride,
	                          bool includeSummary) {
	const std::string message = logger_render_display_message(diagnostic, includeSummary);
	if (message.empty()) {
		return;
	}
	this->emit(OutputAction::DISPLAY_MESSAGE,
	           OutputContext{.message = message,
	                         .source = sourceOverride.empty() ? diagnostic.source : sourceOverride});
}

// ─────────────────────────────────────────────────────────────────────────────
// Display table helpers
// ─────────────────────────────────────────────────────────────────────────────

void Logger::displayTableHeader(const std::vector<std::string>& headers) {
	this->emit(OutputAction::DISPLAY_TABLE_HEADER,
	           OutputContext{.payload = joinPipe(headers)});
}

void Logger::displayTableRow(const std::vector<std::string>& cells) {
	this->emit(OutputAction::DISPLAY_TABLE_ROW,
	           OutputContext{.payload = joinPipe(cells)});
}

void Logger::displayTableEnd() {
	this->emit(OutputAction::DISPLAY_TABLE_END);
}
