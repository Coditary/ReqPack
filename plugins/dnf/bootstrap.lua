function bootstrap()
    local markerPath = REQPACK_PLUGIN_DIR .. "/.bootstrap_done"
    local marker = io.open(markerPath, "r")
    if marker ~= nil then
        marker:close()
        return true
    end

    local handle = io.popen("which dnf 2>/dev/null")
    local result = handle:read("*a")
    handle:close()

    if result == "" then
        print("[Lua: DNF] Bootstrap: dnf ist nicht installiert. Hier koennte Erstinstallation stattfinden.")
        return false
    end

    local bootstrapMarker = io.open(markerPath, "w")
    if bootstrapMarker ~= nil then
        bootstrapMarker:write("done\n")
        bootstrapMarker:close()
    end

    return true
end
