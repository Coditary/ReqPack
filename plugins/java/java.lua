plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function normalized_target(value)
    local target = trim(value):lower()
    if target == "" then
        return nil
    end
    return target
end

local function choose_target(context)
    local proxy = context.proxy or {}
    local target = normalized_target(proxy.default)
    if target ~= nil then
        return target
    end

    local targets = proxy.targets or {}
    for _, candidate in ipairs(targets) do
        target = normalized_target(candidate)
        if target ~= nil then
            return target
        end
    end

    return "maven"
end

local function unresolved_result(context, operation)
    context.log.error("java proxy must resolve before '" .. operation .. "'")
    return false
end

function plugin.getName()
    return "ReqPack Java Proxy Manager"
end

function plugin.getVersion()
    return "1.0.0"
end

function plugin.getRequirements()
    return {}
end

function plugin.getCategories()
    return { "Proxy", "Java" }
end

function plugin.getMissingPackages(packages)
    return packages or {}
end

function plugin.install(context, packages)
    return unresolved_result(context, "install")
end

function plugin.installLocal(context, path)
    return unresolved_result(context, "installLocal")
end

function plugin.remove(context, packages)
    return unresolved_result(context, "remove")
end

function plugin.update(context, packages)
    return unresolved_result(context, "update")
end

function plugin.list(context)
    return {}
end

function plugin.outdated(context)
    return {}
end

function plugin.search(context, prompt)
    return {}
end

function plugin.info(context, package)
    return {
        name = package or "java",
        version = "proxy",
        description = "Java proxy manager",
    }
end

function plugin.resolveProxyRequest(context, request)
    local target = choose_target(context)
    if target == nil then
        context.log.error("java proxy could not choose target")
        return nil
    end

    local resolution = {
        targetSystem = target,
        flags = request.flags,
    }
    if request.usesLocalTarget then
        resolution.localPath = request.localPath
    else
        resolution.packages = request.packages
    end
    return resolution
end

function plugin.shutdown()
    return true
end
