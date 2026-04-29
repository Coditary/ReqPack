#pragma once

#include "output/idisplay.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// OutputAction — identifies how a queued event should be processed.
//
// Display-prefixed actions drive IDisplay; Plugin-prefixed actions originate
// from plugin runtime callbacks.
// ─────────────────────────────────────────────────────────────────────────────
enum class OutputAction {
	// ── Logging / raw stdout ─────────────────────────────────────────────────
	LOG,               ///< Route through spdlog.
	STDOUT,            ///< Write raw message to stdout.

	// ── Plugin runtime callbacks ─────────────────────────────────────────────
	PLUGIN_STATUS,     ///< emitStatus()   → IDisplay::onItemProgress (status)
	PLUGIN_PROGRESS,   ///< emitProgress() → IDisplay::onItemProgress
	PLUGIN_EVENT,      ///< emitEvent()    → IDisplay::onMessage
	PLUGIN_ARTIFACT,   ///< registerArtifact() → IDisplay::onMessage

	// ── Display session lifecycle ─────────────────────────────────────────────
	/// Signals start of a command.
	/// context: displayMode = (int)DisplayMode, payload = pipe-delimited ids.
	DISPLAY_SESSION_BEGIN,

	/// Signals end of a command.
	/// context: statusCode = 0 ok / 1 fail,
	///          payload    = "<succeeded>:<failed>"
	DISPLAY_SESSION_END,

	// ── Display item lifecycle ────────────────────────────────────────────────
	/// context: source = itemId, message = display label.
	DISPLAY_ITEM_BEGIN,

	/// context: source = itemId, message = step label.
	DISPLAY_ITEM_STEP,

	/// context: source = itemId.
	DISPLAY_ITEM_SUCCESS,

	/// context: source = itemId, message = failure reason.
	DISPLAY_ITEM_FAILURE,

	// ── Display table output ─────────────────────────────────────────────────
	/// context: payload = pipe-delimited column headers.
	DISPLAY_TABLE_HEADER,

	/// context: payload = pipe-delimited cell values.
	DISPLAY_TABLE_ROW,

	DISPLAY_TABLE_END,

	// ── Control ──────────────────────────────────────────────────────────────
	FLUSH,
	STOP
};

// ─────────────────────────────────────────────────────────────────────────────
// OutputContext — payload attached to every queued OutputEvent.
// ─────────────────────────────────────────────────────────────────────────────
struct OutputContext {
	spdlog::level::level_enum level{spdlog::level::info};
	std::string               message{};
	std::string               source{};     ///< Plugin id / item id.
	std::string               scope{};      ///< Package name / sub-scope.
	int                       statusCode{0};
	int                       progressPercent{0};
	std::string               eventName{};
	std::string               payload{};    ///< Multi-purpose encoded data.
	int                       displayMode{0}; ///< Cast of DisplayMode enum.
};

struct OutputEvent {
	OutputAction  action{OutputAction::LOG};
	OutputContext context{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger — async single-consumer event queue + display routing.
//
// Usage:
//   Logger::instance().setDisplay(std::make_unique<PlainDisplay>());
//   Logger::instance().displaySessionBegin(DisplayMode::INSTALL, {"curl","wget"});
//   Logger::instance().emit(OutputAction::PLUGIN_PROGRESS, {...});
//   Logger::instance().displaySessionEnd(true, 2, 0);
// ─────────────────────────────────────────────────────────────────────────────
class Logger {
	std::shared_ptr<spdlog::logger> logger;
	std::vector<spdlog::sink_ptr>   sinks;
	/// Reference to the console sink so its level can be muted when a display renderer is active.
	std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> consoleSink;
	std::thread                     worker;
	std::deque<OutputEvent>         queue;
	std::mutex                      queueMutex;
	std::condition_variable         queueCondition;
	bool                            stopRequested{false};
	std::string                     pattern{"%^[%T] [%l] %v%$"};

	/// IDisplay implementation; accessed only on the worker thread after set.
	std::atomic<IDisplay*>          display{nullptr};

	Logger(const Logger&)            = delete;
	Logger& operator=(const Logger&) = delete;

	void        startWorker();
	void        enqueue(OutputEvent event);
	void        processLoop();
	void        processEvent(const OutputEvent& event);
	static std::string formatMessage(const OutputContext& context);

	/// Route an OutputEvent to the active IDisplay (called on worker thread).
	void routeToDisplay(const OutputEvent& event);

public:
	Logger();
	~Logger();

	static Logger& instance();

	// ── Configuration ─────────────────────────────────────────────────────────

	void setLevel(const std::string& level);
	void setLevel(spdlog::level::level_enum level);
	void setPattern(const std::string& pattern);
	void setFileSink(const std::string& filename);
	void setBacktrace(bool enable, size_t max_size = 10);

	/// Attach an IDisplay implementation.  Ownership stays with the caller;
	/// the pointer must outlive all emit() calls.
	void setDisplay(IDisplay* d);

	// ── Core emit ─────────────────────────────────────────────────────────────

	void emit(OutputAction action, const OutputContext& context = {});
	void stdout(const std::string& message,
	            const std::string& source = {},
	            const std::string& scope  = {});
	void flush();

	// ── Spdlog convenience wrappers ───────────────────────────────────────────

	void crit (const std::string& message);
	void err  (const std::string& message);
	void warn (const std::string& message);
	void info (const std::string& message);
	void debug(const std::string& message);
	void trace(const std::string& message);

	// ── Display session helpers ───────────────────────────────────────────────

	/// Signal start of a top-level command.
	void displaySessionBegin(DisplayMode                    mode,
	                          const std::vector<std::string>& items);

	/// Signal end of a top-level command.
	void displaySessionEnd(bool success, int succeeded, int skipped, int failed);

	// ── Display item helpers ──────────────────────────────────────────────────

	void displayItemBegin  (const std::string& itemId, const std::string& label);
	void displayItemStep   (const std::string& itemId, const std::string& step);
	void displayItemSuccess(const std::string& itemId);
	void displayItemFailure(const std::string& itemId, const std::string& reason);

	// ── Display table helpers ─────────────────────────────────────────────────

	void displayTableHeader(const std::vector<std::string>& headers);
	void displayTableRow   (const std::vector<std::string>& cells);
	void displayTableEnd   ();
};
