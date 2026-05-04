#pragma once

#include <string>
#include <vector>

#include "output/progress_metrics.h"

// ─────────────────────────────────────────────────────────────────────────────
// DisplayMode — mirrors ActionType but lives in output layer, no boost dep.
// ─────────────────────────────────────────────────────────────────────────────
enum class DisplayMode {
	IDLE,
	INSTALL,
	REMOVE,
	UPDATE,
	SEARCH,
	LIST,
	OUTDATED,
	INFO,
	SNAPSHOT,
	SERVE,
	REMOTE,
	ENSURE,
	SBOM
};

// ─────────────────────────────────────────────────────────────────────────────
// DisplayItemState — lifecycle of a single tracked item (package / plugin).
// ─────────────────────────────────────────────────────────────────────────────
enum class DisplayItemState {
	PENDING,
	RUNNING,
	SUCCESS,
	FAILED
};

// ─────────────────────────────────────────────────────────────────────────────
// DisplayItemStatus — snapshot of one item, kept by IDisplay implementations.
// ─────────────────────────────────────────────────────────────────────────────
struct DisplayItemStatus {
	std::string      id;
	std::string      label;
	DisplayProgressMetrics metrics{};
	std::string      step;
	DisplayItemState state{DisplayItemState::PENDING};
};

// ─────────────────────────────────────────────────────────────────────────────
// IDisplay — abstract CLI renderer.
//
// Lifecycle contract:
//   onSessionBegin  → [onItemBegin → onItemProgress* → onItemStep* →
//                      onItemSuccess | onItemFailure]*
//                   → onSessionEnd
//
// Table contract (for LIST / SEARCH):
//   onTableBegin → onTableRow* → onTableEnd
//
// Implementations receive all calls on the Logger worker thread; they must
// either be thread-safe internally or rely on the single-consumer guarantee.
// ─────────────────────────────────────────────────────────────────────────────
class IDisplay {
public:
	virtual ~IDisplay() = default;

	// ── Session ──────────────────────────────────────────────────────────────

	/// Called once when a command starts.
	/// @param mode     Which operation is running (INSTALL, LIST, …).
	/// @param items    Ordered list of item IDs that will be processed.
	virtual void onSessionBegin(DisplayMode mode,
	                            const std::vector<std::string>& items) = 0;

	/// Called once when the command finishes (success or partial/full failure).
	/// @param success    True when every item succeeded.
	/// @param succeeded  Number of items that completed successfully.
	/// @param skipped    Number of items that were skipped.
	/// @param failed     Number of items that failed.
	virtual void onSessionEnd(bool success, int succeeded, int skipped, int failed) = 0;

	// ── Per-item ──────────────────────────────────────────────────────────────

	/// Item transitions from PENDING → RUNNING.
	/// @param itemId   Unique identifier (package name, plugin id, …).
	/// @param label    Human-readable display name.
	virtual void onItemBegin(const std::string& itemId,
	                         const std::string& label) = 0;

	/// Progress snapshot.
	virtual void onItemProgress(const std::string& itemId,
	                            const DisplayProgressMetrics& metrics) = 0;

	/// Plugin entered a new named step (e.g. "Downloading", "Extracting").
	virtual void onItemStep(const std::string& itemId,
	                        const std::string& step) = 0;

	/// Item completed successfully; progress implied 100 %.
	virtual void onItemSuccess(const std::string& itemId) = 0;

	/// Item failed.
	/// @param reason  Optional human-readable failure description.
	virtual void onItemFailure(const std::string& itemId,
	                           const std::string& reason) = 0;

	// ── Generic output ────────────────────────────────────────────────────────

	/// Informational / diagnostic line that does not belong to a single item.
	/// @param source  Optional label shown before the message (plugin id, etc.).
	virtual void onMessage(const std::string& text,
	                       const std::string& source = {}) = 0;

	// ── Table output (LIST / SEARCH / INFO) ───────────────────────────────────

	/// Start a table; defines column headers and count.
	virtual void onTableBegin(const std::vector<std::string>& headers) = 0;

	/// One data row; cells align with the previously declared headers.
	virtual void onTableRow(const std::vector<std::string>& cells) = 0;

	/// Finalise the table (draw footer, spacing, etc.).
	virtual void onTableEnd() = 0;

	// ── Housekeeping ──────────────────────────────────────────────────────────

	virtual void flush() = 0;
};
