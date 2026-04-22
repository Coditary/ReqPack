plugin = {
    name = "Fedora DNF Manager",
    version = "1.2.0",
    author = "Leodora",
    description = "Echtes DNF Plugin fuer Fedora"
}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function command_succeeds(cmd)
    local success, _, code = os.execute(cmd)
    return success == true or success == 0 or code == 0
end

local function command_exit_code(cmd)
    local success, _, code = os.execute(cmd)
    if success == true or success == 0 then
        return 0
    end

    return tonumber(code) or 1
end

local function sys_call(cmd)
    print("[DNF-Exec] " .. cmd)
    return command_succeeds(cmd)
end

local function package_spec(pkg)
    if pkg.version == nil or pkg.version == "" then
        return pkg.name
    end

    return pkg.name .. "-" .. pkg.version
end

local function package_installed(name)
    return command_succeeds("rpm -q --quiet " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function package_has_update(name)
    return command_exit_code("dnf check-update --quiet " .. shell_quote(name) .. " >/dev/null 2>&1") == 100
end

local function command_exists(name)
    local handle = io.popen("command -v " .. name .. " 2>/dev/null")
    if handle == nil then
        return false
    end

    local result = handle:read("*a") or ""
    handle:close()
    return trim(result) ~= ""
end

function plugin.init()
    if not command_exists("dnf") then
        print("[Lua: DNF] Fehler: dnf wurde auf diesem System nicht gefunden!")
        return false
    end
    return true
end

function plugin.getCategories()
    return { "System", "RPM", "Fedora Native" }
end

function plugin.getMissingPackages(packages)
    local missing = {}
    for _, pkg in ipairs(packages or {}) do
        local action = pkg.action
        local installed = package_installed(package_spec(pkg))
        if action == "remove" or action == 2 then
            if package_installed(pkg.name) then
                table.insert(missing, pkg)
            end
        elseif action == "update" or action == 3 then
            if pkg.version ~= nil and pkg.version ~= "" then
                if not installed then
                    table.insert(missing, pkg)
                end
            elseif package_installed(pkg.name) and package_has_update(pkg.name) then
                table.insert(missing, pkg)
            end
        elseif not installed then
            table.insert(missing, pkg)
        end
    end

    return missing
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
        for _, pkg in ipairs(packages) do table.insert(names, package_spec(pkg)) end
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
