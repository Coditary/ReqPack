plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

function plugin.getName()
    return "demo-system-plugin"
end

function plugin.getVersion()
    return "1.0.0"
end

function plugin.getRequirements()
    return {}
end

function plugin.getCategories()
    return { "test", "system" }
end

function plugin.getMissingPackages(packages)
    return packages or {}
end

function plugin.install(context, packages)
    context.tx.begin_step("install packages")
    local result = context.exec.run("demo-pm install " .. packages[1].name)
    if not result.success then
        context.tx.failed("install failed")
        return false
    end
    context.events.installed({ name = packages[1].name })
    context.tx.success()
    return true
end

function plugin.installLocal(context, path)
    local result = context.exec.run("demo-pm install-local " .. path)
    return result.success
end

function plugin.remove(context, packages)
    local result = context.exec.run("demo-pm remove " .. packages[1].name)
    if result.success then
        context.events.deleted({ name = packages[1].name })
    end
    return result.success
end

function plugin.update(context, packages)
    local result = context.exec.run("demo-pm update " .. packages[1].name)
    if result.success then
        context.events.updated({ name = packages[1].name })
    end
    return result.success
end

function plugin.list(context)
    local result = context.exec.run("demo-pm list")
    if not result.success then
        return {}
    end
    local items = {
        { name = "alpha", version = "1.2.3", description = trim(result.stdout) }
    }
    context.events.listed(items)
    return items
end

function plugin.outdated(context)
    local items = {
        { name = "alpha", version = "1.2.3", latestVersion = "1.2.4" }
    }
    context.events.outdated(items)
    return items
end

function plugin.search(context, prompt)
    local result = context.exec.run("demo-pm search " .. prompt)
    if not result.success then
        return {}
    end
    local items = {
        { name = trim(prompt), version = "9.9.9", summary = trim(result.stdout) }
    }
    context.events.searched(items)
    return items
end

function plugin.info(context, package_name)
    local result = context.exec.run("demo-pm info " .. package_name)
    if not result.success then
        return {}
    end
    local item = {
        name = trim(package_name),
        version = "4.5.6",
        description = trim(result.stdout),
    }
    context.events.informed(item)
    return item
end

function plugin.init()
    return true
end

function plugin.shutdown()
    return true
end

return plugin
