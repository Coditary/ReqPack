#include "audit_exporter_internal.h"

#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

DiagnosticMessage audit_output_diagnostic(const std::string& summary, const std::string& cause, const std::string& recommendation) {
    return make_error_diagnostic(
        "audit",
        summary,
        cause,
        recommendation,
        {},
        "audit",
        "output"
    );
}

bool request_has_flag(const Request& request, const std::string& name) {
    return std::find(request.flags.begin(), request.flags.end(), name) != request.flags.end();
}

bool write_output_file(const std::string& rendered, const Request& request, const std::string& outputPath, const bool interactive) {
    std::filesystem::path filePath(outputPath);
    if (filePath.is_relative()) {
        filePath = std::filesystem::current_path() / filePath;
    }
    const std::string resolvedOutputPath = filePath.string();

    if (std::filesystem::exists(filePath)) {
        const bool force = request_has_flag(request, "force");
        if (!force) {
            Logger& logger = Logger::instance();
            if (!interactive) {
                return false;
            }
            logger.stdout(resolvedOutputPath + " already exists. Overwrite? [y/N]");
            logger.flushSync();
            std::string answer;
            if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
                logger.stdout("aborted.");
                logger.flushSync();
                return false;
            }
        }
    }

    std::error_code error;
    const std::filesystem::path parentPath = filePath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, error);
        if (error) {
            Logger::instance().diagnostic(audit_output_diagnostic(
                "failed to create audit output directory: " + resolvedOutputPath,
                "ReqPack could not create parent directory for audit export output.",
                "Check target path permissions and parent directory state, then retry."
            ));
            Logger::instance().flushSync();
            return false;
        }
    }

    std::ofstream output(resolvedOutputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        Logger::instance().diagnostic(audit_output_diagnostic(
            "failed to open audit output path: " + resolvedOutputPath,
            "ReqPack could not open requested audit output file for writing.",
            "Check whether path points to writable file location and retry."
        ));
        Logger::instance().flushSync();
        return false;
    }

    output << rendered;
    if (!output.good()) {
        Logger::instance().diagnostic(audit_output_diagnostic(
            "failed to write audit output path: " + resolvedOutputPath,
            "ReqPack could not finish writing audit export to output file.",
            "Check disk space, filesystem health, and write permissions, then retry."
        ));
        Logger::instance().flushSync();
        return false;
    }

    std::cout << resolvedOutputPath << '\n';
    std::cout.flush();
    return true;
}

}  // namespace

bool AuditExporter::exportGraph(const Graph& graph, const std::vector<ValidationFinding>& findings, const Request& request) const {
    const std::string rendered = renderGraph(graph, findings, request);
    const std::string outputPath = resolveOutputPath(request);
    if (outputPath.empty()) {
        std::cout << rendered;
        if (rendered.empty() || rendered.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
        return true;
    }

    return write_output_file(rendered, request, outputPath, this->config.interaction.interactive);
}
