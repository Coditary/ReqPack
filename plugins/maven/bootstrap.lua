function bootstrap()
    local function command_exists(name)
        local handle = io.popen("command -v " .. name .. " 2>/dev/null")
        if handle == nil then
            return false
        end

        local result = handle:read("*a") or ""
        handle:close()
        return result:gsub("%s+", "") ~= ""
    end

    if not command_exists("java") then
        print("[Lua: Maven] Bootstrap: java fehlt noch.")
        return false
    end

    if not command_exists("mvn") then
        print("[Lua: Maven] Bootstrap: mvn fehlt noch.")
        return false
    end

    return true
end
