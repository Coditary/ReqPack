plugin = {}

function plugin.getName() return "Trivy" end
function plugin.getVersion() return "0.1.0" end
function plugin.getSecurityMetadata()
  return {
    role = "security-provider",
    ecosystemScopes = { "Maven", "crates.io", "Debian", "RPM" },
    privilegeLevel = "none",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "security-provider" } end
function plugin.getMissingPackages(packages) return packages end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return true end
function plugin.remove(context, packages) return true end
function plugin.update(context, packages) return true end
function plugin.list(context) return {} end
function plugin.outdated(context) return {} end
function plugin.search(context, prompt) return {} end
function plugin.info(context, package) return { name = package, version = "0.1.0", description = "security provider" } end
function plugin.shutdown() return true end

return plugin
