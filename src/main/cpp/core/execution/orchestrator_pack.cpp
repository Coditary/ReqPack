#include "orchestrator_internal.h"

#include "core/host/host_info.h"
#include "core/packages/rq_package.h"
#include "output/command_output.h"
#include "output/diagnostic.h"
#include "output/logger.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

bool request_has_flag(const Request& request, const std::string& flag) {
	return std::find(request.flags.begin(), request.flags.end(), flag) != request.flags.end();
}

CommandOutput builtin_pack_output(const RqPackageBuildResult& result) {
	CommandOutput output;
	output.mode = DisplayMode::PACK;
	output.sessionItems = {result.metadata.name};
	output.success = true;
	output.succeeded = 1;
	output.blocks.push_back(make_command_field_value_block({
		{.key = "System", .value = "rqp"},
		{.key = "Format", .value = "rqp"},
		{.key = "Package", .value = result.metadata.name},
		{.key = "Version", .value = result.metadata.version + "-" + std::to_string(result.metadata.release) + "+r" + std::to_string(result.metadata.revision)},
		{.key = "Output Path", .value = result.outputPath.string()},
	}));
	output.blocks.push_back(make_command_artifact_block("artifact", result.outputPath.string()));
	return output;
}

CommandOutput plugin_pack_output(const std::string& system, const std::vector<std::string>& artifacts) {
	CommandOutput output;
	output.mode = DisplayMode::PACK;
	output.sessionItems = {system};
	output.success = true;
	output.succeeded = 1;
	output.blocks.push_back(make_command_field_value_block({
		{.key = "System", .value = system},
		{.key = "Artifacts", .value = std::to_string(artifacts.size())},
	}));
	for (const std::string& artifact : artifacts) {
		output.blocks.push_back(make_command_artifact_block("artifact", artifact));
	}
	return output;
}

} // namespace

namespace orchestrator_internal {

int run_pack_request(const Request& request, Registry* registry, const ReqPackConfig& config) {
	try {
		if (request.system.empty()) {
			RqPackageBuildRequest buildRequest;
			buildRequest.projectRoot = request.localPath;
			buildRequest.outputPath = request.outputPath;
			buildRequest.force = request_has_flag(request, "force");
			buildRequest.interactive = config.interaction.interactive;
			if (!request.payloadPath.empty()) {
				buildRequest.payloadRoot = std::filesystem::path(request.payloadPath);
			}

			const RqPackageBuildResult result = rq_build_package(buildRequest, config);
			render_command_output(builtin_pack_output(result));
			return 0;
		}

		if (registry->getPlugin(request.system) == nullptr || !registry->loadPlugin(request.system)) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"pack",
				"Plugin load failed for system '" + request.system + "'",
				"ReqPack could not load requested plugin before running pack operation.",
				"Check plugin installation and registry state, then retry.",
				{},
				request.system,
				"pack"
			));
			return 1;
		}

		IPlugin* plugin = registry->getPlugin(request.system);
		if (plugin == nullptr || !plugin->supportsPack()) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"pack",
				"system '" + request.system + "' does not support pack",
				"Selected plugin does not expose optional pack capability.",
				"Use builtin `rqp pack <project-dir>` or choose plugin that implements pack.",
				{},
				request.system,
				"pack"
			));
			return 1;
		}

		PluginCallContext context{
			.pluginId = plugin->getPluginId(),
			.pluginDirectory = plugin->getPluginDirectory(),
			.scriptPath = plugin->getScriptPath(),
			.flags = request.flags,
			.host = plugin->getRuntimeHost(),
			.proxy = proxy_config_for_system(config, plugin->getPluginId()),
			.currentItemId = request.system,
			.repositories = repositories_for_ecosystem(config, plugin->getPluginId()),
			.hostInfo = HostInfoService::currentSnapshot(),
		};

		std::vector<std::string> packFlags = request.flags;
		if (!request.payloadPath.empty()) {
			packFlags.push_back("payload-dir=" + request.payloadPath);
		}
		const bool ok = plugin->pack(context, request.localPath, request.outputPath, packFlags);
		std::vector<std::string> artifacts = plugin->takeRecentArtifacts();
		if (ok && artifacts.empty() && !request.outputPath.empty()) {
			const std::filesystem::path explicitOutput = std::filesystem::path(request.outputPath).is_relative()
				? (std::filesystem::current_path() / request.outputPath)
				: std::filesystem::path(request.outputPath);
			if (std::filesystem::exists(explicitOutput)) {
				artifacts.push_back(explicitOutput.string());
			}
		}
		if (ok && request.outputPath.empty() && artifacts.empty()) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"pack",
				"plugin pack succeeded but did not register any output artifact",
				"Plugin reported success, but ReqPack received no artifact path to report back.",
				"Update plugin.pack to register produced artifact with context.artifacts.register(...).",
				{},
				request.system,
				"pack"
			));
			return 1;
		}
		if (ok && !request.outputPath.empty() && artifacts.empty()) {
			Logger::instance().diagnostic(make_error_diagnostic(
				"pack",
				"plugin pack succeeded but output artifact is missing: " + request.outputPath,
				"Plugin reported success for explicit output path, but resulting artifact file was not found.",
				"Check plugin pack implementation and output path handling, then retry.",
				{},
				request.system,
				"pack"
			));
			return 1;
		}
		if (!ok) {
			return 1;
		}
		render_command_output(plugin_pack_output(request.system, artifacts));
		return 0;
	} catch (const std::exception& error) {
		Logger::instance().diagnostic(make_error_diagnostic(
			"pack",
			error.what(),
			"Pack command failed before producing valid artifact.",
			"Check package layout, plugin support, and output path, then retry.",
			{},
			request.system.empty() ? "rqp" : request.system,
			"pack"
		));
		return 1;
	}
}

} // namespace orchestrator_internal
