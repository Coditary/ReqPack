#include "core/history/history_manager.h"

#include "history_manager_internal.h"

#include <filesystem>

HistoryManager::HistoryManager(const ReqPackConfig& cfg) : config(cfg) {}

std::filesystem::path HistoryManager::historyDir() const {
	if (!config.history.historyPath.empty()) {
		return std::filesystem::path(config.history.historyPath);
	}
	return default_reqpack_history_path();
}

std::filesystem::path HistoryManager::historyLogPath() const {
	return historyDir() / "history.jsonl";
}

std::filesystem::path HistoryManager::legacyInstalledStatePath() const {
	return historyDir() / "installed.json";
}

std::filesystem::path HistoryManager::installedStateDatabasePath() const {
	return historyDir();
}

bool HistoryManager::ensureDirectory() const {
	const std::filesystem::path dir = historyDir();
	if (std::filesystem::exists(dir)) {
		return true;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	return !ec;
}

bool HistoryManager::record(const HistoryEntry& entry) const {
	HistoryEntry filled = entry;
	if (filled.timestamp.empty()) {
		filled.timestamp = history_manager_internal::utc_timestamp_now();
	}

	bool ok = appendEvent(filled);
	ok = updateInstalledState(filled) && ok;
	return ok;
}
