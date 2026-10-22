#include "core/config/configuration.h"
#include "core/registry/registry.h"
#include "core/security/security_bridge.h"
#include "output/logger.h"

#include <rqp/security/security_status.h>

#include <sstream>

int run_security_status(const ReqPackConfig& config, Logger& logger) {
    Registry registry(config);
    (void)registry.getDatabase()->ensureReady();

    const SecuritySettings settings = security_settings_from(config);
    const SecurityStatusReport report = build_security_status(settings, &registry);

    std::ostringstream buffer;
    print_security_status_report(report, buffer);
    logger.logStdout(buffer.str());
    logger.flushSync();
    return report.hasBlockingIssues ? 1 : 0;
}
