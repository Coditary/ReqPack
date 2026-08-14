plugin = {}

function plugin.getName() return "GitHub Advisory" end
function plugin.getVersion() return "0.1.0" end
function plugin.getSecurityMetadata()
  return {
    role = "security-provider",
    capabilities = { "sync" },
    ecosystemScopes = { "all-github-advisory-ecosystems" },
    networkScopes = {
      { host = "codeload.github.com", scheme = "https", pathPrefix = "/github/advisory-database/" },
      { host = "github.com", scheme = "https", pathPrefix = "/github/advisory-database/" },
    },
    privilegeLevel = "none",
  }
end
function plugin.getRequirements() return {} end
function plugin.getCategories() return { "Security", "Security Provider", "GitHub Advisory" } end
function plugin.getMissingPackages(packages) return packages or {} end
function plugin.install(context, packages) return true end
function plugin.installLocal(context, path) return false end
function plugin.remove(context, packages) return false end
function plugin.update(context, packages) return true end
function plugin.list(context)
  return {
    { name = "github-advisories", version = "0.1.0", type = "security-database", summary = "Sync all ecosystems available in GitHub Advisory Database" },
  }
end
function plugin.outdated(context) return {} end
function plugin.search(context, prompt)
  return {
    { name = "github-advisories", version = "0.1.0", type = "security-database", summary = "Sync all ecosystems exposed by GitHub Advisory Database" },
  }
end
function plugin.info(context, name)
  return {
    name = tostring(name or ""),
    version = "0.1.0",
    type = "security-database",
    summary = "GitHub Advisory Database sync target",
    description = "Imports OSV-formatted advisories from github/advisory-database into ReqPack shared vulnerability indexes for requested ecosystems.",
  }
end
function plugin.shutdown() return true end

return plugin
