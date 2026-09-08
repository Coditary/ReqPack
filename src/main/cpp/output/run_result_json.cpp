#include "output/run_result_json.h"

#include "output/logger.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

namespace {

std::string json_escape(const std::string& value) {
	std::string escaped;
	escaped.reserve(value.size());
	for (const char c : value) {
		switch (c) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buffer[8];
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
					escaped += buffer;
				} else {
					escaped.push_back(c);
				}
				break;
		}
	}
	return escaped;
}

}  // namespace

std::string run_result_status_for_action(ActionType action, bool success, bool dryRun) {
	if (!success) {
		return "failed";
	}
	if (dryRun) {
		return "planned";
	}
	switch (action) {
		case ActionType::REMOVE:
			return "removed";
		case ActionType::UPDATE:
			return "updated";
		case ActionType::ENSURE:
		case ActionType::INSTALL:
		default:
			return "installed";
	}
}

std::string run_result_status_skipped(ActionType action) {
	(void)action;
	return "skipped";
}

std::string serialize_run_result_json(const RunResultJsonDocument& document) {
	std::map<std::string, std::vector<const RunResultJsonItem*>> bySystem;
	for (const RunResultJsonItem& item : document.items) {
		bySystem[item.system].push_back(&item);
	}

	std::ostringstream stream;
	stream << "{";
	stream << "\"ok\":" << (document.ok ? "true" : "false");
	stream << ",\"dryRun\":" << (document.dryRun ? "true" : "false");

	for (const auto& [system, items] : bySystem) {
		stream << ",\"" << json_escape(system) << "\":[";
		for (std::size_t index = 0; index < items.size(); ++index) {
			const RunResultJsonItem& item = *items[index];
			if (index > 0) {
				stream << ",";
			}
			stream << "{";
			stream << "\"name\":\"" << json_escape(item.name) << "\"";
			stream << ",\"version\":\"" << json_escape(item.version) << "\"";
			stream << ",\"status\":\"" << json_escape(item.status) << "\"";
			if (!item.errorMessage.empty()) {
				stream << ",\"errorMessage\":\"" << json_escape(item.errorMessage) << "\"";
			}
			stream << "}";
		}
		stream << "]";
	}

	stream << "}";
	return stream.str();
}

void emit_run_result_json(const RunResultJsonDocument& document) {
	Logger::instance().flushSync();
	std::cout << serialize_run_result_json(document) << '\n';
	std::cout.flush();
}
