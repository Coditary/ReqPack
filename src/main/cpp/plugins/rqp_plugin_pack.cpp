#include "rqp_plugin_internal.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

bool RqpPlugin::pack(const PluginCallContext& context,
                     const std::string& projectPath,
                     const std::string& outputPath,
                     const std::vector<std::string>& flags) {
    recentEvents_.clear();
    recentArtifacts_.clear();

    const bool force = std::find(flags.begin(), flags.end(), "force") != flags.end();
    const auto payloadFlag = std::find_if(flags.begin(), flags.end(), [](const std::string& flag) {
        return flag.rfind("payload-dir=", 0) == 0;
    });

    RqPackageBuildRequest request;
    request.projectRoot = projectPath;
    request.outputPath = outputPath;
    request.force = force;
    request.interactive = config_.interaction.interactive;
    if (payloadFlag != flags.end() && payloadFlag->size() > 12) {
        request.payloadRoot = std::filesystem::path(payloadFlag->substr(12));
    }

    try {
        context.emitBeginStep("build rqp package");
        const RqPackageBuildResult result = rq_build_package(request, config_);
        recentArtifacts_.push_back(result.outputPath.string());
        context.registerArtifact(result.outputPath.string());
        context.emitSuccess();
        return true;
    } catch (const std::exception& error) {
        context.emitFailure(error.what());
        return false;
    }
}
