#include "core/history_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string utc_timestamp_now() {
    const auto now  = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Minimal JSON string escaping — no external dependency.
std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string jstr(const std::string& key, const std::string& value) {
    return "\"" + escape_json(key) + "\":\"" + escape_json(value) + "\"";
}

// Extract the value of a JSON string field from a flat (single-line) JSON object.
// Returns empty string when the key is absent.
std::string extract_json_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const std::size_t start = pos + needle.size();
    std::string value;
    bool escaped = false;
    for (std::size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) {
            switch (c) {
                case '"':  value += '"';  break;
                case '\\': value += '\\'; break;
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                default:   value += c;    break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            value += c;
        }
    }
    return value;
}

// Read all non-empty lines from a file.
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

// Overwrite a file with the given lines (each terminated by '\n').
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

// ─────────────────────────────────────────────────────────────────────────────
// HistoryManager
// ─────────────────────────────────────────────────────────────────────────────

HistoryManager::HistoryManager(const ReqPackConfig& cfg) : config(cfg) {}

std::filesystem::path HistoryManager::historyDir() const {
    // config.history.historyPath is already tilde-expanded by configuration.cpp.
    if (!config.history.historyPath.empty()) {
        return std::filesystem::path(config.history.historyPath);
    }
    return reqpack_home_directory() / "history";
}

std::filesystem::path HistoryManager::historyLogPath() const {
    return historyDir() / "history.jsonl";
}

std::filesystem::path HistoryManager::installedStatePath() const {
    return historyDir() / "installed.json";
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

// ── JSON helpers ──────────────────────────────────────────────────────────────

std::string HistoryManager::escapeJson(const std::string& s) {
    return escape_json(s);
}

// Format:
//   {"timestamp":"…","action":"…","manager":"…","package":"…","version":"…","status":"…"}
// The "error" field is only included when there is an actual error message,
// keeping successful entries lean.
std::string HistoryManager::entryToJsonLine(const HistoryEntry& e) {
    std::string line;
    line.reserve(200);
    line += '{';
    line += jstr("timestamp", e.timestamp); line += ',';
    line += jstr("action",    e.action);    line += ',';
    line += jstr("manager",   e.system);    line += ',';
    line += jstr("package",   e.packageName); line += ',';
    line += jstr("version",   e.packageVersion); line += ',';
    line += jstr("status",    e.status);
    if (!e.errorMessage.empty()) {
        line += ',';
        line += jstr("error", e.errorMessage);
    }
    line += '}';
    return line;
}

// ── Rotation / trimming ───────────────────────────────────────────────────────

void HistoryManager::trimHistoryLog() const {
    const std::size_t maxLines  = config.history.maxLines;
    const double      maxSizeMb = config.history.maxSizeMb;

    if (maxLines == 0 && maxSizeMb <= 0.0) {
        return; // both limits disabled
    }

    const std::filesystem::path path = historyLogPath();
    if (!std::filesystem::exists(path)) {
        return;
    }

    // Check whether any limit is actually breached before loading everything.
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

    // Load lines if not already loaded (size-based trigger).
    if (lines.empty()) {
        lines = read_all_lines(path);
    }
    if (lines.empty()) {
        return;
    }

    // Determine how many lines to keep.
    // Use the stricter of the two active limits.
    std::size_t keepCount = lines.size(); // start: keep all

    if (maxLines > 0 && keepCount > maxLines) {
        keepCount = maxLines;
    }

    if (maxSizeMb > 0.0) {
        // Estimate: walk from the end and accumulate sizes until we exceed the limit.
        const std::size_t maxBytes = static_cast<std::size_t>(maxSizeMb * 1024.0 * 1024.0);
        std::size_t total = 0;
        std::size_t sizeKeep = 0;
        for (std::size_t i = lines.size(); i > 0; --i) {
            total += lines[i - 1].size() + 1; // +1 for '\n'
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
        return; // nothing to drop
    }

    // Keep the newest entries (tail of the vector).
    const std::size_t dropCount = lines.size() - keepCount;
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(dropCount));

    (void)write_all_lines(path, lines);
}

// ── Installed-state helpers ───────────────────────────────────────────────────

std::vector<InstalledEntry> HistoryManager::loadInstalledState() const {
    std::vector<InstalledEntry> entries;
    const std::filesystem::path path = installedStatePath();
    if (!std::filesystem::exists(path)) {
        return entries;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return entries;
    }

    // File is a JSON array written by saveInstalledState, one object per line.
    // We identify object lines by the presence of "name": and extract fields.
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"name\"") == std::string::npos) {
            continue;
        }
        InstalledEntry entry;
        entry.name        = extract_json_string(line, "name");
        entry.version     = extract_json_string(line, "version");
        entry.system      = extract_json_string(line, "manager");
        entry.installedAt = extract_json_string(line, "installedAt");
        if (!entry.name.empty() && !entry.system.empty()) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool HistoryManager::saveInstalledState(const std::vector<InstalledEntry>& entries) const {
    const std::filesystem::path path = installedStatePath();
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    // Pretty-printed JSON array; each object on one line for easy grep/parsing.
    file << "[\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const InstalledEntry& e = entries[i];
        file << "  {";
        file << jstr("name",        e.name)        << ',';
        file << jstr("version",     e.version)     << ',';
        file << jstr("manager",     e.system)      << ',';
        file << jstr("installedAt", e.installedAt);
        file << '}';
        if (i + 1 < entries.size()) {
            file << ',';
        }
        file << '\n';
    }
    file << "]\n";
    return file.good();
}

// ── Public API ────────────────────────────────────────────────────────────────

bool HistoryManager::appendEvent(const HistoryEntry& entry) const {
    // Guard: only write the event log when history.enabled is true.
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

bool HistoryManager::updateInstalledState(const HistoryEntry& entry) const {
    // Guard: only update the snapshot when history.trackInstalled is true.
    if (!config.history.trackInstalled) {
        return true;
    }
    // Only state-changing actions that succeeded affect the snapshot.
    if (entry.status != "success") {
        return true;
    }

    const bool isInstall = entry.action == "install" || entry.action == "ensure";
    const bool isRemove  = entry.action == "remove";
    const bool isUpdate  = entry.action == "update";

    if (!isInstall && !isRemove && !isUpdate) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!ensureDirectory()) {
        return false;
    }

    std::vector<InstalledEntry> entries = loadInstalledState();

    if (isRemove) {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(), [&](const InstalledEntry& e) {
                return e.system == entry.system && e.name == entry.packageName;
            }),
            entries.end()
        );
    } else {
        // Install or update: upsert.
        bool found = false;
        for (InstalledEntry& e : entries) {
            if (e.system == entry.system && e.name == entry.packageName) {
                e.version     = entry.packageVersion;
                e.installedAt = entry.timestamp;
                found = true;
                break;
            }
        }
        if (!found) {
            entries.push_back(InstalledEntry{
                .name        = entry.packageName,
                .version     = entry.packageVersion,
                .system      = entry.system,
                .installedAt = entry.timestamp
            });
        }
    }

    return saveInstalledState(entries);
}

bool HistoryManager::record(const HistoryEntry& entry) const {
    // Inject timestamp if caller left it empty.
    HistoryEntry filled = entry;
    if (filled.timestamp.empty()) {
        filled.timestamp = utc_timestamp_now();
    }

    // appendEvent and updateInstalledState each check their own flag internally.
    bool ok = appendEvent(filled);
    ok = updateInstalledState(filled) && ok;
    return ok;
}
