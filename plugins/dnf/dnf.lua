plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function package_spec(pkg)
    if pkg.version == nil or pkg.version == "" then
        return pkg.name
    end

    return pkg.name .. "-" .. pkg.version
end

local function command_succeeds(cmd)
    local result = reqpack.exec.run(cmd)
    return result.success
end

local function command_stdout(cmd)
    local result = reqpack.exec.run(cmd)
    return trim(result.stdout or "")
end

local function command_exit_code(cmd)
    local result = reqpack.exec.run(cmd)
    return result.exitCode or 1
end

local function package_installed(name)
    return command_succeeds("rpm -q --quiet " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function package_request_installed(pkg)
    if pkg.version ~= nil and pkg.version ~= "" then
        return package_installed(package_spec(pkg))
    end

    return command_succeeds("rpm -q --quiet --whatprovides " .. shell_quote(pkg.name) .. " >/dev/null 2>&1")
end

local function package_resolvable(pkg)
    local spec = package_spec(pkg)
    if command_stdout("dnf repoquery --quiet " .. shell_quote(spec) .. " 2>/dev/null") ~= "" then
        return true
    end

    if pkg.version == nil or pkg.version == "" then
        return command_stdout("dnf repoquery --quiet --whatprovides " .. shell_quote(pkg.name) .. " 2>/dev/null") ~= ""
    end

    return false
end

local function package_specs(packages)
    local names = {}
    for _, pkg in ipairs(packages or {}) do
        table.insert(names, package_spec(pkg))
    end
    return names
end

local function shell_join(values)
    local quoted = {}
    for _, value in ipairs(values or {}) do
        table.insert(quoted, shell_quote(value))
    end
    return table.concat(quoted, " ")
end

local function package_has_update(name)
    return command_exit_code("dnf check-update --quiet " .. shell_quote(name) .. " >/dev/null 2>&1") == 100
end

function plugin.getName()
    return "Fedora DNF Manager"
end

function plugin.getVersion()
    return "2.0.0"
end

function plugin.getSecurityMetadata()
    return {
        purlType = "rpm",
        versionComparatorProfile = "rpm-evr",
    }
end

function plugin.getCategories()
    return { "System", "RPM", "Fedora Native" }
end

plugin.fileExtensions = { ".rpm" }

function plugin.getMissingPackages(packages)
    local missing = {}
    for _, pkg in ipairs(packages or {}) do
        if pkg.localTarget then
            table.insert(missing, pkg)
        else
            local action = pkg.action
            local installed = package_request_installed(pkg)
            if action == "remove" or action == 2 then
                if installed then
                    table.insert(missing, pkg)
                end
            elseif action == "update" or action == 3 then
                if pkg.version ~= nil and pkg.version ~= "" then
                    if not installed then
                        table.insert(missing, pkg)
                    end
                elseif installed and package_has_update(pkg.name) then
                    table.insert(missing, pkg)
                end
            elseif not installed then
                table.insert(missing, pkg)
            end
        end
    end

    return missing
end

function plugin.getRequirements()
    return {}
end

function plugin.install(context, packages)
    if #packages == 0 then return true end

    local installable_packages = {}
    local unavailable_packages = {}
    for _, pkg in ipairs(packages) do
        if package_resolvable(pkg) then
            table.insert(installable_packages, pkg)
        else
            table.insert(unavailable_packages, pkg)
        end
    end

    local installable_names = package_specs(installable_packages)
    local unavailable_names = package_specs(unavailable_packages)

    context.tx.begin_step("install dnf packages")
    for _, name in ipairs(unavailable_names) do
        context.events.unavailable(name)
    end
    if #unavailable_names > 0 then
        context.log.warn("unavailable packages skipped from batch: " .. table.concat(unavailable_names, " "))
    end

    if #installable_names > 0 then
        context.log.info("installing batch: " .. table.concat(installable_names, " "))
        local result = context.exec.run("sudo dnf install -y " .. shell_join(installable_names))
        if not result.success then
            context.tx.failed("dnf install failed")
            return false
        end
        context.events.installed(installable_names)
    end

    if #unavailable_names > 0 then
        context.tx.failed("some dnf packages are unavailable")
        return false
    end

    context.tx.success()
    return true
end

function plugin.installLocal(context, path)
    context.tx.begin_step("install local rpm")
    local result = context.exec.run("sudo dnf install -y " .. shell_quote(path))
    if not result.success then
        context.tx.failed("dnf local install failed")
        return false
    end

    context.events.installed({ path = path, localTarget = true })
    context.tx.success()
    return true
end

function plugin.remove(context, packages)
    if #packages == 0 then return true end

    local names = {}
    for _, pkg in ipairs(packages) do table.insert(names, pkg.name) end

    context.tx.begin_step("remove dnf packages")
    local result = context.exec.run("sudo dnf remove -y " .. shell_join(names))
    if not result.success then
        context.tx.failed("dnf remove failed")
        return false
    end

    context.events.deleted(names)
    context.tx.success()
    return true
end

function plugin.update(context, packages)
    local cmd = "sudo dnf upgrade -y"
    if packages ~= nil and #packages > 0 then
        cmd = cmd .. " " .. shell_join(package_specs(packages))
    end

    context.tx.begin_step("update dnf packages")
    local result = context.exec.run(cmd)
    if not result.success then
        context.tx.failed("dnf update failed")
        return false
    end

    context.events.updated(packages or {})
    context.tx.success()
    return true
end

function plugin.list(context)
    local result = context.exec.run("dnf repoquery --installed --qf $'%{name}\\t%{version}-%{release}\\n'")
    local items = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local name, ver = line:match("^(.-)\t(.+)$")
        if name and ver then
            table.insert(items, { name = name, version = ver, description = "Installed RPM" })
        end
    end
    context.events.listed(items)
    return items
end

function plugin.outdated(context)
    -- dnf check-update exits 100 when updates available, 0 when none, non-zero on error
    local result = context.exec.run("dnf check-update --quiet 2>/dev/null; echo \"EXIT:$?\"")
    local stdout = result.stdout or ""
    local exit_code = tonumber(stdout:match("EXIT:(%d+)$")) or 1
    if exit_code ~= 0 and exit_code ~= 100 then
        context.log.warn("dnf check-update failed with exit code " .. tostring(exit_code))
        return {}
    end
    local items = {}
    for line in stdout:gmatch("[^\r\n]+") do
        if line:match("^EXIT:") then break end
        -- output format: "name.arch    new-version    repo"
        local name, ver = line:match("^(%S+)%s+(%S+)%s")
        if name and ver then
            -- strip architecture suffix (e.g. ".x86_64", ".noarch")
            local baseName = name:match("^(.-)%.[^.]+$") or name
            table.insert(items, {
                name = baseName,
                version = ver,
                description = "Update available"
            })
        end
    end
    context.events.outdated(items)
    return items
end

function plugin.search(context, prompt)
    local result = context.exec.run("dnf search " .. shell_quote(prompt) .. " --quiet")
    local items = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local name = line:match("^(%S+)")
        if name ~= nil and name ~= "Last" and name ~= "Matched" then
            table.insert(items, {
                name = name,
                version = "repo",
                description = trim(line)
            })
        end
    end
    context.events.searched(items)
    return items
end

function plugin.info(context, name)
    local result = context.exec.run("dnf info " .. shell_quote(name) .. " --quiet")
    local description = trim(result.stdout or "")
    local item = { name = name, version = "unknown", description = description ~= "" and description or "DNF Package" }
    context.events.informed(item)
    return item
end

function plugin.init()
    return reqpack.exec.run("command -v dnf >/dev/null 2>&1").success
end

function plugin.shutdown()
    return true
end
