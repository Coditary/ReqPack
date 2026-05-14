#include "history_manager_internal.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string escape_json(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (const char c : s) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out += c; break;
		}
	}
	return out;
}

std::string jstr(const std::string& key, const std::string& value) {
	return "\"" + escape_json(key) + "\":\"" + escape_json(value) + "\"";
}

std::vector<std::string> read_all_lines(const std::filesystem::path& path) {
	std::vector<std::string> lines;
	std::ifstream file(path);
	if (!file.is_open()) {
		return lines;
	}
	std::string line;
	while (std::getline(file, line)) {
		if (!line.empty()) {
			lines.push_back(std::move(line));
		}
	}
	return lines;
}

bool write_all_lines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
	std::ofstream file(path, std::ios::trunc);
	if (!file.is_open()) {
		return false;
	}
	for (const std::string& line : lines) {
		file << line << '\n';
	}
	return file.good();
}

} // namespace

namespace history_manager_internal {

std::string utc_timestamp_now() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
	gmtime_r(&t, &tm);
	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
	return oss.str();
}

} // namespace history_manager_internal

std::string HistoryManager::escapeJson(const std::string& s) {
	return escape_json(s);
}

std::string HistoryManager::entryToJsonLine(const HistoryEntry& e) {
	std::string line;
	line.reserve(200);
	line += '{';
	line += jstr("timestamp", e.timestamp); line += ',';
	line += jstr("action", e.action); line += ',';
	line += jstr("manager", e.system); line += ',';
	line += jstr("package", e.packageName); line += ',';
	line += jstr("version", e.packageVersion); line += ',';
	line += jstr("status", e.status);
	if (!e.errorMessage.empty()) {
		line += ',';
		line += jstr("error", e.errorMessage);
	}
	line += '}';
	return line;
}

void HistoryManager::trimHistoryLog() const {
	const std::size_t maxLines = config.history.maxLines;
	const double maxSizeMb = config.history.maxSizeMb;

	if (maxLines == 0 && maxSizeMb <= 0.0) {
		return;
	}

	const std::filesystem::path path = historyLogPath();
	if (!std::filesystem::exists(path)) {
		return;
	}

	bool breached = false;
	if (maxSizeMb > 0.0) {
		std::error_code ec;
		const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
		if (!ec) {
			const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
			if (mb > maxSizeMb) {
				breached = true;
			}
		}
	}

	std::vector<std::string> lines;
	if (maxLines > 0 && !breached) {
		lines = read_all_lines(path);
		if (lines.size() > maxLines) {
			breached = true;
		}
	}

	if (!breached) {
		return;
	}

	if (lines.empty()) {
		lines = read_all_lines(path);
	}
	if (lines.empty()) {
		return;
	}

	std::size_t keepCount = lines.size();
	if (maxLines > 0 && keepCount > maxLines) {
		keepCount = maxLines;
	}

	if (maxSizeMb > 0.0) {
		const std::size_t maxBytes = static_cast<std::size_t>(maxSizeMb * 1024.0 * 1024.0);
		std::size_t total = 0;
		std::size_t sizeKeep = 0;
		for (std::size_t i = lines.size(); i > 0; --i) {
			total += lines[i - 1].size() + 1;
			if (total <= maxBytes) {
				sizeKeep = lines.size() - (i - 1);
			} else {
				break;
			}
		}
		if (sizeKeep < keepCount) {
			keepCount = sizeKeep;
		}
	}

	if (keepCount >= lines.size()) {
		return;
	}

	const std::size_t dropCount = lines.size() - keepCount;
	lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(dropCount));

	(void)write_all_lines(path, lines);
}

bool HistoryManager::appendEvent(const HistoryEntry& entry) const {
	if (!config.history.enabled) {
		return true;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (!ensureDirectory()) {
		return false;
	}

	{
		std::ofstream file(historyLogPath(), std::ios::app);
		if (!file.is_open()) {
			return false;
		}
		file << entryToJsonLine(entry) << '\n';
		if (!file.good()) {
			return false;
		}
	}

	trimHistoryLog();
	return true;
}
