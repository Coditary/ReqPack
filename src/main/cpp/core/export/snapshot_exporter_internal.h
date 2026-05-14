#pragma once

#include "core/export/snapshot_exporter.h"

#include <string>
#include <vector>

namespace snapshot_exporter_internal {

std::vector<InstalledEntry> load_sorted_snapshot_entries(const ReqPackConfig& config);
std::string build_snapshot_rendered_text(const std::vector<InstalledEntry>& entries);
std::string resolved_output_path(const Request& request);

bool write_snapshot_output(
	const ReqPackConfig& config,
	const Request& request,
	const std::vector<InstalledEntry>& entries,
	const std::string& rendered,
	const std::string& outputPath
);

} // namespace snapshot_exporter_internal
