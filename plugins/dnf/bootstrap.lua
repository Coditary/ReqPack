function bootstrap()
    local marker = io.open(REQPACK_PLUGIN_DIR .. "/bootstrapped.txt", "w")
    if marker ~= nil then
        marker:write("bootstrapped\n")
        marker:close()
    end
    return true
end
