#include "history_manager_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool owner_has_prefix(const std::string& owner, std::string_view prefix) {
	return history_manager_internal::starts_with(owner, std::string(prefix) + "\n");
}

} // namespace

namespace history_manager_internal {

bool starts_with(const std::string& value, std::string_view prefix) {
	return value.rfind(prefix, 0) == 0;
}

std::string escape_field(const std::string& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		if (c == '\\' || c == '\n') {
			escaped.push_back('\\');
			escaped.push_back(c == '\n' ? 'n' : c);
			continue;
		}
		escaped.push_back(c);
	}
	return escaped;
}

std::string unescape_field(const std::string& value) {
	std::string unescaped;
	unescaped.reserve(value.size());

	bool escaped = false;
	for (char c : value) {
		if (!escaped && c == '\\') {
			escaped = true;
			continue;
		}
		if (escaped) {
			unescaped.push_back(c == 'n' ? '\n' : c);
			escaped = false;
			continue;
		}
		unescaped.push_back(c);
	}

	return unescaped;
}

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
				case '"': value += '"'; break;
				case '\\': value += '\\'; break;
				case 'n': value += '\n'; break;
				case 'r': value += '\r'; break;
				case 't': value += '\t'; break;
				default: value += c; break;
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

std::vector<InstalledEntry> read_legacy_installed_state(const std::filesystem::path& path) {
	std::vector<InstalledEntry> entries;
	if (!std::filesystem::exists(path)) {
		return entries;
	}

	std::ifstream file(path);
	if (!file.is_open()) {
		return entries;
	}

	std::string line;
	while (std::getline(file, line)) {
		if (line.find("\"name\"") == std::string::npos) {
			continue;
		}
		InstalledEntry entry;
		entry.name = extract_json_string(line, "name");
		entry.version = extract_json_string(line, "version");
		entry.system = extract_json_string(line, "manager");
		entry.installedAt = extract_json_string(line, "installedAt");
		if (!entry.name.empty() && !entry.system.empty()) {
			entries.push_back(std::move(entry));
		}
	}
	return entries;
}

std::string serialize_installed_entry(const InstalledEntry& entry) {
	std::ostringstream stream;
	stream << "name=" << escape_field(entry.name) << '\n';
	stream << "version=" << escape_field(entry.version) << '\n';
	stream << "system=" << escape_field(entry.system) << '\n';
	stream << "installedAt=" << escape_field(entry.installedAt) << '\n';
	stream << "installMethod=" << escape_field(entry.installMethod);
	for (const std::string& owner : entry.owners) {
		stream << '\n' << "owner=" << escape_field(owner);
	}
	return stream.str();
}

std::vector<std::string> normalize_owners(std::vector<std::string> owners) {
	owners.erase(std::remove_if(owners.begin(), owners.end(), [](const std::string& owner) {
		return owner.empty();
	}), owners.end());
	std::sort(owners.begin(), owners.end());
	owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
	return owners;
}

std::string install_method_from_owners(const std::vector<std::string>& owners) {
	const bool hasExplicit = std::any_of(owners.begin(), owners.end(), [](const std::string& owner) {
		return owner_has_prefix(owner, "root");
	});
	const bool hasDependency = std::any_of(owners.begin(), owners.end(), [](const std::string& owner) {
		return owner_has_prefix(owner, "pkg");
	});

	if (hasExplicit && hasDependency) {
		return "explicit+dependency";
	}
	if (hasExplicit) {
		return "explicit";
	}
	if (hasDependency) {
		return "dependency";
	}
	return "unknown";
}

std::optional<InstalledEntry> deserialize_installed_entry(const std::string& payload) {
	InstalledEntry entry;
	std::istringstream stream(payload);
	std::string line;
	while (std::getline(stream, line)) {
		const std::size_t equals = line.find('=');
		if (equals == std::string::npos) {
			continue;
		}

		const std::string key = line.substr(0, equals);
		const std::string value = unescape_field(line.substr(equals + 1));
		if (key == "name") {
			entry.name = value;
		} else if (key == "version") {
			entry.version = value;
		} else if (key == "system") {
			entry.system = value;
		} else if (key == "installedAt") {
			entry.installedAt = value;
		} else if (key == "installMethod") {
			entry.installMethod = value;
		} else if (key == "owner") {
			entry.owners.push_back(value);
		}
	}

	if (entry.name.empty() || entry.system.empty()) {
		return std::nullopt;
	}
	entry.owners = normalize_owners(std::move(entry.owners));
	if (entry.installMethod.empty()) {
		entry.installMethod = install_method_from_owners(entry.owners);
	}
	return entry;
}

std::string installed_state_key(const std::string& system, const std::string& name, const std::string& version) {
	return std::string("pkg:") + escape_field(system) + '\n' + escape_field(name) + '\n' + escape_field(version);
}

std::string installed_state_system_prefix(const std::string& system) {
	return std::string("pkg:") + escape_field(system) + '\n';
}

std::string installed_state_name_prefix(const std::string& system, const std::string& name) {
	return installed_state_system_prefix(system) + escape_field(name) + '\n';
}

std::string installed_state_identity(const InstalledEntry& entry) {
	return escape_field(entry.name) + '\n' + escape_field(entry.version);
}

} // namespace history_manager_internal
