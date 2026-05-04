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

local function installed_package_version(name)
    local version = command_stdout("rpm -q --qf '%{VERSION}-%{RELEASE}\n' " .. shell_quote(name) .. " 2>/dev/null")
    if version ~= "" and not version:match("not installed") then
        return trim(version:match("([^\r\n]+)"))
    end

    local providers = command_stdout("rpm -q --whatprovides --qf '%{NAME}\t%{VERSION}-%{RELEASE}\n' " .. shell_quote(name) .. " 2>/dev/null")
    for line in providers:gmatch("[^\r\n]+") do
        local provider_name, provider_version = line:match("^(.-)\t(.+)$")
        if provider_name ~= nil and provider_name ~= "" and provider_version ~= nil and provider_version ~= "" then
            return trim(provider_version)
        end
    end

    return ""
end

local function shell_join(values)
    local quoted = {}
    for _, value in ipairs(values or {}) do
        table.insert(quoted, shell_quote(value))
    end
    return table.concat(quoted, " ")
end

local function normalize_flag_value(value)
    return trim(string.lower(tostring(value or "")))
end

local function search_filters(flags)
    local filters = {
        arch = {},
        type = {},
    }
    for _, flag in ipairs(flags or {}) do
        local arch = tostring(flag):match("^arch=(.+)$")
        if arch ~= nil and arch ~= "" then
            filters.arch[normalize_flag_value(arch)] = true
        end
        local package_type = tostring(flag):match("^type=(.+)$")
        if package_type ~= nil and package_type ~= "" then
            filters.type[normalize_flag_value(package_type)] = true
        end
    end
    return filters
end

local function table_has_entries(values)
    return next(values) ~= nil
end

local function dnf_package_type(name)
    local normalized = normalize_flag_value(name)
    if normalized:match("%-devel$") then return "devel" end
    if normalized:match("%-doc$") then return "doc" end
    if normalized:match("%-libs?$") then return "libs" end
    if normalized:match("%-common$") then return "common" end
    if normalized:match("%-tools?$") then return "tools" end
    if normalized:match("%-plugin[s]?$") then return "plugin" end
    if normalized:match("%-cli$") then return "cli" end
    return "package"
end

local function split_package_arch(name)
    local package_name, arch = tostring(name or ""):match("^(.-)%.([^.]+)$")
    if package_name ~= nil and arch ~= nil and arch ~= "" then
        return package_name, arch
    end
    return tostring(name or ""), ""
end

local function collect_search_versions(entries)
    local versions = {}
    local batch = {}

    local function load_batch()
        if #batch == 0 then
            return
        end
        local result = reqpack.exec.run(
            "dnf repoquery --latest-limit=1 --qf '%{name}.%{arch}\t%{version}-%{release}\n' " .. shell_join(batch) .. " 2>/dev/null"
        )
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local trimmed = trim(line)
            if trimmed ~= "" and trimmed ~= "Updating and loading repositories:" and trimmed ~= "Repositories loaded." then
                local name_with_arch, version = trimmed:match("^(.-)\t(.+)$")
                if name_with_arch ~= nil and version ~= nil then
                    versions[name_with_arch] = trim(version)
                end
            end
        end
        batch = {}
    end

    for _, entry in ipairs(entries or {}) do
        batch[#batch + 1] = entry.name_with_arch
        if #batch >= 250 then
            load_batch()
        end
    end
    load_batch()
    return versions
end

local function matches_search_filters(item, filters)
    if table_has_entries(filters.arch) and not filters.arch[normalize_flag_value(item.architecture)] then
        return false
    end
    if table_has_entries(filters.type) and not filters.type[normalize_flag_value(item.type)] then
        return false
    end
    return true
end

local function dnf_progress_rules()
    return {
        initial = "running",
        rules = {
            {
                state = "running",
                source = "line",
                regex = "^Updating and loading repositories:$",
                actions = {
                    { type = "begin_step", label = "load repositories" },
                },
            },
            {
                state = "running",
                source = "line",
                regex = "^Running transaction$",
                actions = {
                    { type = "begin_step", label = "running transaction" },
                },
            },
            {
                state = "running",
                source = "line",
                regex = [[^\[[0-9]+/[0-9]+\]\s+([A-Za-z][A-Za-z ]*[A-Za-z])\s+.+?\s+(\d+)%%\s+\|\s+([0-9.]+)\s*(B/s|[KMGTP]i?B/s)\s+\|\s+([0-9.]+)\s*(B|[KMGTP]i?B)\s+\|\s+.*$]],
                actions = {
                    { type = "begin_step", label = "${1}" },
                    { type = "progress", percent = "${2}", speed = "${3}", speedUnit = "${4}", current = "${5}", currentUnit = "${6}" },
                },
            },
            {
                state = "running",
                source = "line",
                regex = [[^.+?\s+(\d+)%%\s+\|\s+([0-9.]+)\s*(B/s|[KMGTP]i?B/s)\s+\|\s+([0-9.]+)\s*(B|[KMGTP]i?B)\s+\|\s+.*$]],
                actions = {
                    { type = "progress", percent = "${1}", speed = "${2}", speedUnit = "${3}", current = "${4}", currentUnit = "${5}" },
                },
            },
        },
    }
end

local function append_info_field(fields, key, value)
    local text = trim(value)
    if text == "" then
        return
    end
    if fields[key] == nil or fields[key] == "" then
        fields[key] = text
    else
        fields[key] = fields[key] .. " " .. text
    end
end

local function parse_dnf_info_fields(text)
    local fields = {}
    local current_key = nil
    for line in (text or ""):gmatch("[^\r\n]+") do
        local trimmed_line = trim(line)
        if trimmed_line == "Installed packages" then
            fields.__status = "installed"
        elseif trimmed_line == "Available Packages" then
            fields.__status = "available"
        else
            local key, value = line:match("^([%a][%a%s%-]+)%s*:%s*(.*)$")
            if key ~= nil then
                current_key = trim(key)
                append_info_field(fields, current_key, value)
            else
                local continuation = line:match("^%s*:%s*(.*)$")
                if continuation ~= nil and current_key ~= nil then
                    append_info_field(fields, current_key, continuation)
                end
            end
        end
    end
    return fields
end

local function info_version_from_fields(fields)
    local version = trim(fields["Version"] or "")
    local release = trim(fields["Release"] or "")
    local epoch = trim(fields["Epoch"] or "")
    if version ~= "" and release ~= "" then
        version = version .. "-" .. release
    end
    if version ~= "" and epoch ~= "" and epoch ~= "0" then
        version = epoch .. ":" .. version
    end
    return version
end

local function resolve_local_file(path, extension)
    local file_type = reqpack.exec.run("test -d " .. shell_quote(path) .. " && printf dir || printf file")
    if not file_type.success or trim(file_type.stdout or "") ~= "dir" then
        return path, nil
    end

    local result = reqpack.exec.run("find " .. shell_quote(path) .. " -type f -name " .. shell_quote("*" .. extension) .. " 2>/dev/null | sort")
    if not result.success then
        return nil, "failed to inspect extracted local package"
    end

    local matches = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        if trim(line) ~= "" then
            table.insert(matches, trim(line))
        end
    end

    if #matches == 0 then
        return nil, "no local " .. extension .. " package found in extracted archive"
    end
    if #matches > 1 then
        return nil, "multiple local " .. extension .. " packages found in extracted archive"
    end
    return matches[1], nil
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
        local result = context.exec.run("sudo dnf install -y " .. shell_join(installable_names), dnf_progress_rules())
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
    local resolved_path, resolve_error = resolve_local_file(path, ".rpm")
    if resolved_path == nil then
        context.tx.failed(resolve_error)
        return false
    end

    context.tx.begin_step("install local rpm")
    local result = context.exec.run("sudo dnf install -y " .. shell_quote(resolved_path), dnf_progress_rules())
    if not result.success then
        context.tx.failed("dnf local install failed")
        return false
    end

    context.events.installed({ path = resolved_path, localTarget = true })
    context.tx.success()
    return true
end

function plugin.remove(context, packages)
    if #packages == 0 then return true end

    local names = {}
    for _, pkg in ipairs(packages) do table.insert(names, pkg.name) end

    context.tx.begin_step("remove dnf packages")
    local result = context.exec.run("sudo dnf remove -y " .. shell_join(names), dnf_progress_rules())
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
    local result = context.exec.run(cmd, dnf_progress_rules())
    if not result.success then
        context.tx.failed("dnf update failed")
        return false
    end

    context.events.updated(packages or {})
    context.tx.success()
    return true
end

function plugin.list(context)
    local result = context.exec.run("dnf repoquery --installed --qf '%{name}\t%{version}-%{release}\n'")
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
    local filters = search_filters(context.flags)
    local result = context.exec.run("dnf search " .. shell_quote(prompt) .. " --quiet")
    local items = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local entry = trim(line)
        local name_with_arch, summary = entry:match("^(%S+)%s+(.+)$")
        if name_with_arch ~= nil and not entry:match("^Last metadata expiration check:") and not entry:match("^Matched fields:") then
            local package_name, architecture = split_package_arch(name_with_arch)
            local item = {
                name_with_arch = name_with_arch,
                name = package_name,
                version = "repo",
                type = dnf_package_type(package_name),
                architecture = architecture,
                description = trim(summary),
            }
            if matches_search_filters(item, filters) then
                table.insert(items, item)
            end
        end
    end

    local versions = collect_search_versions(items)
    for _, item in ipairs(items) do
        item.version = versions[item.name_with_arch] or item.version
        item.name_with_arch = nil
    end

    context.events.searched(items)
    return items
end

function plugin.info(context, name)
    local result = context.exec.run("dnf info " .. shell_quote(name) .. " --quiet")
    local fields = parse_dnf_info_fields(result.stdout or "")
    local installed_version = installed_package_version(name)
    local version = info_version_from_fields(fields)
    if version == "" then
        version = installed_version
    end

    if next(fields) == nil and version == "" then
        return {}
    end

    local extra_fields = {}
    if trim(fields["Vendor"] or "") ~= "" then
        table.insert(extra_fields, { key = "Vendor", value = trim(fields["Vendor"]) })
    end

    local item = {
        name = trim(fields["Name"] or "") ~= "" and trim(fields["Name"]) or name,
        version = version ~= "" and version or "unknown",
        status = trim(fields.__status or ""),
        installed = installed_version ~= "" and "yes" or "",
        summary = trim(fields["Summary"] or ""),
        description = trim(fields["Description"] or ""),
        homepage = trim(fields["URL"] or ""),
        sourceUrl = trim(fields["Source"] or ""),
        repository = trim(fields["From repository"] or ""),
        architecture = trim(fields["Architecture"] or ""),
        license = trim(fields["License"] or ""),
        size = trim(fields["Size"] or ""),
        installedSize = trim(fields["Installed size"] or ""),
        extraFields = extra_fields,
    }
    if item.summary == "" then
        item.summary = item.description ~= "" and item.description or "DNF Package"
    end
    if item.description == "" then
        item.description = item.summary
    end
    context.events.informed(item)
    return item
end

function plugin.resolvePackage(context, package)
    local spec = package_spec(package)
    if command_stdout("dnf repoquery --quiet " .. shell_quote(spec) .. " 2>/dev/null") == "" then
        return nil
    end

    local resolved = package
    if resolved.version == nil or resolved.version == "" then
        local installed_version = installed_package_version(resolved.name)
        if installed_version ~= "" then
            resolved.version = installed_version
        end
    end
    return resolved
end

function plugin.init()
    return reqpack.exec.run("command -v dnf >/dev/null 2>&1").success
end

function plugin.shutdown()
    return true
end
