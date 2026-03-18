-- fedora_dnf.lua
plugin = {
    name = "Fedora DNF Manager",
    version = "1.1.0",
    author = "Leodora",
    description = "Echtes DNF Plugin für Fedora"
}

-- Hilfsfunktion zum Ausführen von Systembefehlen
local function sys_call(cmd)
    print("[DNF-Exec] " .. cmd)
    local success, exit_type, code = os.execute(cmd)
    return success -- true wenn der Befehl mit 0 beendet wurde
end

function plugin.init()
    -- Prüfen, ob dnf überhaupt da ist
    local handle = io.popen("which dnf 2>/dev/null")
    local result = handle:read("*a")
    handle:close()
    
    if result == "" then
        print("[Lua: DNF] Fehler: dnf wurde auf diesem System nicht gefunden!")
        return false
    end
    
    print("[Lua: DNF] Schnittstelle bereit.")
    return true
end

function plugin.getCategories()
    return { "System", "RPM", "Fedora Native" }
end

function plugin.install(packages)
    if #packages == 0 then return true end

    -- Wir bauen einen Batch-String: "pkg1 pkg2 pkg3"
    local names = {}
    for _, pkg in ipairs(packages) do
        table.insert(names, pkg.name)
    end
    local batch_string = table.concat(names, " ")

    print("[Lua: DNF] Installiere Batch: " .. batch_string)
    
    -- -y für non-interactive
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
    -- Hier nutzen wir io.popen, um den Output von dnf zu lesen
    local cmd = "dnf search " .. prompt .. " --quiet | grep " .. prompt
    local handle = io.popen(cmd)
    local result = handle:read("*a")
    handle:close()

    local results = {}
    -- Sehr einfaches Parsing der Zeilen
    for line in result:gmatch("[^\r\n]+") do
        local name = line:match("^([^%.%s]+)") -- Packagename vor dem Punkt oder Leerzeichen
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
    -- Listet nur installierte user-packages auf (vereinfacht)
    local handle = io.popen("dnf list installed --quiet")
    local results = {}
    -- Überspringe Header und parse Zeilen
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

function plugin.getRequirements() return {} end
function plugin.info(name) 
    return { name = name, version = "unknown", description = "DNF Package" } 
end
