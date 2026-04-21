plugin = {
    name = "Fedora DNF Manager",
    version = "1.2.0",
    author = "Leodora",
    description = "Echtes DNF Plugin fuer Fedora"
}

local function sys_call(cmd)
    print("[DNF-Exec] " .. cmd)
    local success = os.execute(cmd)
    return success
end

local function package_spec(pkg)
    if pkg.version == nil or pkg.version == "" then
        return pkg.name
    end

    return pkg.name .. "-" .. pkg.version
end

function plugin.init()
    local handle = io.popen("which dnf 2>/dev/null")
    local result = handle:read("*a")
    handle:close()

    if result == "" then
        print("[Lua: DNF] Fehler: dnf wurde auf diesem System nicht gefunden!")
        return false
    end
    return true
end

function plugin.getCategories()
    return { "System", "RPM", "Fedora Native" }
end

function plugin.install(packages)
    if #packages == 0 then return true end

    local names = {}
    for _, pkg in ipairs(packages) do
        table.insert(names, package_spec(pkg))
    end
    local batch_string = table.concat(names, " ")

    print("[Lua: DNF] Installiere Batch: " .. batch_string)

    local cmd = "sudo dnf install -y " .. batch_string
    return sys_call(cmd)
end

function plugin.remove(packages)
    if #packages == 0 then return true end

    local names = {}
    for _, pkg in ipairs(packages) do table.insert(names, pkg.name) end

    local cmd = "sudo dnf remove -y " .. table.concat(names, " ")
    return sys_call(cmd)
end

function plugin.search(prompt)
    local cmd = "dnf search " .. prompt .. " --quiet | grep " .. prompt
    local handle = io.popen(cmd)
    local result = handle:read("*a")
    handle:close()

    local results = {}
    for line in result:gmatch("[^\r\n]+") do
        local name = line:match("^([^%.%s]+)")
        if name then
            table.insert(results, {
                name = name,
                version = "repo",
                description = line
            })
        end
    end
    return results
end

function plugin.update(packages)
    local cmd = "sudo dnf upgrade -y"
    if #packages > 0 then
        local names = {}
        for _, pkg in ipairs(packages) do table.insert(names, pkg.name) end
        cmd = cmd .. " " .. table.concat(names, " ")
    end
    return sys_call(cmd)
end

function plugin.list()
    local handle = io.popen("dnf list installed --quiet")
    local results = {}
    for line in handle:lines() do
        local name, ver = line:match("^(%S+)%s+(%S+)")
        if name and ver then
            table.insert(results, { name = name, version = ver, description = "Installed RPM" })
        end
    end
    handle:close()
    return results
end

function plugin.shutdown()
    return true
end

function plugin.getRequirements()
    return {}
end

function plugin.info(name)
    return { name = name, version = "unknown", description = "DNF Package" }
end
