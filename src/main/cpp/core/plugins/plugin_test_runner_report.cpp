#include "core/plugins/plugin_test_runner.h"

#include <sstream>

namespace {

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    std::ostringstream code;
                    code << "\\u" << std::hex << std::uppercase
                         << static_cast<int>((c >> 12) & 0xF)
                         << static_cast<int>((c >> 8) & 0xF)
                         << static_cast<int>((c >> 4) & 0xF)
                         << static_cast<int>(c & 0xF);
                    escaped += code.str();
                } else {
                    escaped.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return escaped;
}

}  // namespace

void print_plugin_test_report(const PluginTestRunReport& report, std::ostream& output) {
    for (const PluginTestCaseSummary& entry : report.cases) {
        output << (entry.success ? "[PASS] " : "[FAIL] ") << entry.name;
        if (!entry.success && !entry.message.empty()) {
            output << " - " << entry.message;
        }
        output << '\n';
        if (!entry.artifacts.empty()) {
            output << "  artifacts: " << entry.artifacts.size() << '\n';
        }
    }
    output << "Cases: " << (report.passed + report.failed)
           << ", Passed: " << report.passed
           << ", Failed: " << report.failed << '\n';
}

std::string plugin_test_report_to_json(const PluginTestRunReport& report) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"plugin\": \"" << json_escape(report.pluginId) << "\",\n";
    stream << "  \"pluginPath\": \"" << json_escape(report.pluginPath) << "\",\n";
    stream << "  \"passed\": " << report.passed << ",\n";
    stream << "  \"failed\": " << report.failed << ",\n";
    stream << "  \"cases\": [\n";
    for (std::size_t index = 0; index < report.cases.size(); ++index) {
        const PluginTestCaseSummary& entry = report.cases[index];
        stream << "    {\n";
        stream << "      \"name\": \"" << json_escape(entry.name) << "\",\n";
        stream << "      \"success\": " << (entry.success ? "true" : "false") << ",\n";
        stream << "      \"message\": \"" << json_escape(entry.message) << "\",\n";
        stream << "      \"commands\": [";
        for (std::size_t commandIndex = 0; commandIndex < entry.commands.size(); ++commandIndex) {
            if (commandIndex != 0) {
                stream << ", ";
            }
            stream << '"' << json_escape(entry.commands[commandIndex]) << '"';
        }
        stream << "],\n";
        stream << "      \"stdout\": [";
        for (std::size_t stdoutIndex = 0; stdoutIndex < entry.stdout.size(); ++stdoutIndex) {
            if (stdoutIndex != 0) {
                stream << ", ";
            }
            stream << '"' << json_escape(entry.stdout[stdoutIndex]) << '"';
        }
        stream << "],\n";
        stream << "      \"stderr\": [";
        for (std::size_t stderrIndex = 0; stderrIndex < entry.stderr.size(); ++stderrIndex) {
            if (stderrIndex != 0) {
                stream << ", ";
            }
            stream << '"' << json_escape(entry.stderr[stderrIndex]) << '"';
        }
        stream << "],\n";
        stream << "      \"events\": [";
        for (std::size_t eventIndex = 0; eventIndex < entry.events.size(); ++eventIndex) {
            if (eventIndex != 0) {
                stream << ", ";
            }
            stream << '"' << json_escape(entry.events[eventIndex]) << '"';
        }
        stream << "],\n";
        stream << "      \"artifacts\": [";
        for (std::size_t artifactIndex = 0; artifactIndex < entry.artifacts.size(); ++artifactIndex) {
            if (artifactIndex != 0) {
                stream << ", ";
            }
            stream << '"' << json_escape(entry.artifacts[artifactIndex]) << '"';
        }
        stream << "],\n";
        stream << "      \"eventRecords\": [";
        for (std::size_t eventRecordIndex = 0; eventRecordIndex < entry.eventRecords.size(); ++eventRecordIndex) {
            if (eventRecordIndex != 0) {
                stream << ", ";
            }
            stream << "{\"name\":\"" << json_escape(entry.eventRecords[eventRecordIndex].name)
                   << "\",\"payload\":\"" << json_escape(entry.eventRecords[eventRecordIndex].payload) << "\"}";
        }
        stream << "]\n";
        stream << "    }";
        if (index + 1 != report.cases.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]\n";
    stream << "}\n";
    return stream.str();
}
