#include "snapshot_exporter_internal.h"

#include "output/command_output.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

DiagnosticMessage snapshot_output_diagnostic(const std::string& summary, const std::string& cause, const std::string& recommendation) {
	return make_error_diagnostic(
		"snapshot",
		summary,
		cause,
		recommendation,
		{},
		"snapshot",
		"output"
	);
}

} // namespace

namespace snapshot_exporter_internal {

bool write_snapshot_output(
	const ReqPackConfig& config,
	const Request& request,
	const std::vector<InstalledEntry>& entries,
	const std::string& rendered,
	const std::string& outputPath
) {
	if (entries.empty()) {
		Logger::instance().emit(OutputAction::DISPLAY_MESSAGE,
			OutputContext{.message = "snapshot: no installed packages tracked in history. Ensure history.trackInstalled is enabled in your config."});
	}

	if (outputPath.empty()) {
		render_command_output(SnapshotExporter(config).buildSnapshotOutput(request));
		return true;
	}

	std::filesystem::path filePath(outputPath);
	if (filePath.is_relative()) {
		filePath = std::filesystem::current_path() / filePath;
	}
	const std::string resolvedPath = filePath.string();

	if (std::filesystem::exists(filePath)) {
		const bool force = std::find(request.flags.begin(), request.flags.end(), "force") != request.flags.end();
		if (!force) {
			Logger& logger = Logger::instance();
			logger.stdout(resolvedPath + " already exists. Overwrite? [y/N]");
			logger.flushSync();
			std::string answer;
			if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
				logger.stdout("aborted.");
				logger.flushSync();
				return false;
			}
		}
	}

	std::error_code ec;
	const std::filesystem::path parentPath = filePath.parent_path();
	if (!parentPath.empty()) {
		std::filesystem::create_directories(parentPath, ec);
		if (ec) {
			Logger::instance().diagnostic(snapshot_output_diagnostic(
				"snapshot: failed to create output directory: " + resolvedPath,
				"ReqPack could not create parent directory for snapshot output.",
				"Check target path permissions and parent directory state, then retry."
			));
			Logger::instance().flushSync();
			return false;
		}
	}

	std::ofstream output(resolvedPath, std::ios::binary | std::ios::trunc);
	if (!output.is_open()) {
		Logger::instance().diagnostic(snapshot_output_diagnostic(
			"snapshot: failed to open output path: " + resolvedPath,
			"ReqPack could not open requested snapshot output file for writing.",
			"Check whether path points to writable file location and retry."
		));
		Logger::instance().flushSync();
		return false;
	}

	output << rendered;
	if (!output.good()) {
		Logger::instance().diagnostic(snapshot_output_diagnostic(
			"snapshot: failed to write output path: " + resolvedPath,
			"ReqPack could not finish writing snapshot manifest to output file.",
			"Check disk space, filesystem health, and write permissions, then retry."
		));
		Logger::instance().flushSync();
		return false;
	}

	render_command_output(SnapshotExporter(config).buildSnapshotOutput(request));
	return true;
}

} // namespace snapshot_exporter_internal

bool SnapshotExporter::exportSnapshot(const Request& request) const {
	const std::vector<InstalledEntry> entries = snapshot_exporter_internal::load_sorted_snapshot_entries(this->config);
	const std::string rendered = render(entries);
	const std::string outputPath = resolveOutputPath(request);
	return snapshot_exporter_internal::write_snapshot_output(this->config, request, entries, rendered, outputPath);
}
