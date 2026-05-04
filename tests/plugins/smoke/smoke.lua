plugin = {}

local function result_item(success_name, failure_name, result)
    return {
        name = result.success and success_name or failure_name,
        version = tostring(result.exitCode),
        description = result.stderr ~= "" and result.stderr or result.stdout,
    }
end

function plugin.getName()
    return "smoke"
end

function plugin.getVersion()
    return "1.0.0"
end

function plugin.getRequirements()
    return {}
end

function plugin.getCategories()
    return { "test" }
end

function plugin.getMissingPackages(packages)
    return {}
end

function plugin.install(context, packages)
    return true
end

function plugin.installLocal(context, path)
    return true
end

function plugin.remove(context, packages)
    return true
end

function plugin.update(context, packages)
    return true
end

function plugin.list(context)
    return {}
end

function plugin.outdated(context)
    return {}
end

function plugin.search(context, prompt)
    if prompt == "plain" then
        local result = context.exec.run("python3 -c \"print('plain fallback')\"")
        return { result_item("plain-ok", "plain-failed", result) }
    end

    if prompt == "line" then
        local result = context.exec.run("python3 -c \"print('Progress: 7%'); print('Progress: 42%')\"", {
            rules = {
                {
                    source = "line",
                    regex = "^Progress:\\s+(\\d+)%$",
                    actions = {
                        { type = "progress", percent = "${1}" },
                        { type = "event", name = "line_progress", payload = "${1}" },
                    },
                },
            },
        })
        return { result_item("line-ok", "line-failed", result) }
    end

    if prompt == "screen" then
        local cmd = "python3 -c \"import sys; sys.stdout.write('Continue? [y/N]: '); sys.stdout.flush(); answer=sys.stdin.readline().strip(); sys.stdout.write('ACK=' + answer + '\\n'); sys.stdout.write('Progress: 99%\\n'); sys.stdout.flush()\""
        local result = context.exec.run(cmd, {
            initial = "confirm",
            rules = {
                {
                    state = "confirm",
                    source = "screen",
                    regex = "Continue\\? \\[[A-Za-z]/[A-Za-z]\\]:\\s*$",
                    ["repeat"] = false,
                    stop = true,
                    actions = {
                        { type = "send", value = "y\n" },
                        { type = "state", value = "running" },
                        { type = "log", level = "info", message = "sent confirm" },
                    },
                },
                {
                    state = "running",
                    source = "line",
                    regex = "^ACK=(.+)$",
                    ["repeat"] = false,
                    actions = {
                        { type = "event", name = "ack", payload = "${1}" },
                        { type = "begin_step", label = "ack ${1}" },
                    },
                },
                {
                    state = "running",
                    source = "line",
                    regex = "^Progress:\\s+(\\d+)%$",
                    actions = {
                        { type = "progress", percent = "${1}" },
                        { type = "success" },
                    },
                },
            },
        })
        return { result_item("screen-ok", "screen-failed", result) }
    end

    if prompt == "control" then
        local result = context.exec.run("python3 -c \"print('TOKEN'); print('TOKEN')\"", {
            initial = "default",
            rules = {
                {
                    state = "default",
                    source = "line",
                    regex = "^TOKEN$",
                    ["repeat"] = false,
                    stop = true,
                    actions = {
                        { type = "event", name = "first", payload = "one" },
                        { type = "state", value = "done" },
                    },
                },
                {
                    state = "default",
                    source = "line",
                    regex = "^TOKEN$",
                    actions = {
                        { type = "event", name = "second", payload = "two" },
                    },
                },
            },
        })
        return { result_item("control-ok", "control-failed", result) }
    end

    if prompt == "rich" then
        local result = context.exec.run("python3 -c \"print('loaded:16.4/40.0@2.5')\"", {
            rules = {
                {
                    source = "line",
                    regex = "^loaded:(\\d+\\.\\d+)/(\\d+\\.\\d+)@(\\d+\\.\\d+)$",
                    actions = {
                        { type = "progress", current = "${1}", currentUnit = "MiB", total = "${2}", totalUnit = "MiB", speed = "${3}", speedUnit = "MiB/s" },
                        { type = "event", name = "line_progress", payload = "${1}/${2}@${3}" },
                    },
                },
            },
        })
        return { result_item("rich-ok", "rich-failed", result) }
    end

    if prompt == "invalid" then
        local result = context.exec.run("python3 -c \"print('noop')\"", {
            rules = {
                {
                    source = "broken",
                    regex = "noop",
                    actions = {
                        { type = "log", message = "should not run" },
                    },
                },
            },
        })
        return { result_item("invalid-failed", "invalid-ok", result) }
    end

    return {
        {
            name = "unknown-prompt",
            version = "1",
            description = prompt,
        },
    }
end

function plugin.info(context, package)
    return {
        name = package,
        version = "1",
        description = "smoke",
    }
end
