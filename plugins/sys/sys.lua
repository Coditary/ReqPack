plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function command_succeeds(cmd)
    local result = reqpack.exec.run(cmd)
    return result.success
end

local function command_stdout(cmd)
    local result = reqpack.exec.run(cmd)
    return trim(result.stdout or "")
end

local function getenv(name, fallback)
    local result = reqpack.exec.run("printf '%s' \"${" .. name .. ":-}\"")
    local value = trim(result.stdout or "")
    if value == "" then
        return fallback
    end
    return value
end

local function env_is_true(name)
    local value = trim(getenv(name, "")):lower()
    return value == "1" or value == "true" or value == "yes" or value == "on"
end

local function binary_is_available(binary)
    if binary == nil or trim(binary) == "" then
        return false
    end
    if tostring(binary):find("/", 1, true) ~= nil then
        return command_succeeds("test -x " .. shell_quote(binary))
    end
    return command_succeeds("command -v " .. shell_quote(binary) .. " >/dev/null 2>&1")
end

local function apt_binary()
    return getenv("REQPACK_SYS_APT_BIN", "apt-get")
end

local function apt_cache_binary()
    return getenv("REQPACK_SYS_APT_CACHE_BIN", "apt-cache")
end

local function apt_list_binary()
    return getenv("REQPACK_SYS_APT_LIST_BIN", "apt")
end

local function dpkg_query_binary()
    return getenv("REQPACK_SYS_DPKG_QUERY_BIN", "dpkg-query")
end

local function dnf_binary()
    return getenv("REQPACK_SYS_DNF_BIN", "dnf")
end

local function yum_binary()
    return getenv("REQPACK_SYS_YUM_BIN", "yum")
end

local function zypper_binary()
    return getenv("REQPACK_SYS_ZYPPER_BIN", "zypper")
end

local function pacman_binary()
    return getenv("REQPACK_SYS_PACMAN_BIN", "pacman")
end

local function apk_binary()
    return getenv("REQPACK_SYS_APK_BIN", "apk")
end

local function emerge_binary()
    return getenv("REQPACK_SYS_EMERGE_BIN", "emerge")
end

local function qlist_binary()
    return getenv("REQPACK_SYS_QLIST_BIN", "qlist")
end

local function equery_binary()
    return getenv("REQPACK_SYS_EQUERY_BIN", "equery")
end

local function xbps_install_binary()
    return getenv("REQPACK_SYS_XBPS_INSTALL_BIN", "xbps-install")
end

local function xbps_remove_binary()
    return getenv("REQPACK_SYS_XBPS_REMOVE_BIN", "xbps-remove")
end

local function xbps_query_binary()
    return getenv("REQPACK_SYS_XBPS_QUERY_BIN", "xbps-query")
end

local function eopkg_binary()
    return getenv("REQPACK_SYS_EOPKG_BIN", "eopkg")
end

local function urpmi_binary()
    return getenv("REQPACK_SYS_URPMI_BIN", "urpmi")
end

local function urpme_binary()
    return getenv("REQPACK_SYS_URPME_BIN", "urpme")
end

local function curl_binary()
    return getenv("REQPACK_SYS_CURL_BIN", "curl")
end

local function configured_nix_binary()
    return trim(getenv("REQPACK_SYS_NIX_BIN", ""))
end

local function nix_binary()
    local configured = configured_nix_binary()
    if configured ~= "" then
        return configured
    end

    local home = trim(getenv("HOME", ""))
    local candidates = {}
    if home ~= "" then
        table.insert(candidates, home .. "/.nix-profile/bin/nix-env")
    end
    table.insert(candidates, "/nix/var/nix/profiles/default/bin/nix-env")
    table.insert(candidates, "nix-env")

    for _, candidate in ipairs(candidates) do
        if binary_is_available(candidate) then
            return candidate
        end
    end

    if home ~= "" then
        return home .. "/.nix-profile/bin/nix-env"
    end
    return "nix-env"
end

local function rpm_binary()
    return getenv("REQPACK_SYS_RPM_BIN", "rpm")
end

local function brew_binary()
    return getenv("REQPACK_SYS_BREW_BIN", "brew")
end

local function sudo_binary()
    return getenv("REQPACK_SYS_SUDO_BIN", "sudo")
end

local LOGICAL_PACKAGES = {
    java = {
        apt = "default-jdk",
        dnf = "java-21-openjdk-devel",
        yum = "java-21-openjdk-devel",
        zypper = "java-17-openjdk-devel",
        pacman = "jdk-openjdk",
        apk = "openjdk17",
        emerge = "dev-java/openjdk",
        xbps = "openjdk17",
        eopkg = "openjdk-17",
        urpmi = "java-17-openjdk-devel",
        nix = "jdk",
        brew = "openjdk",
        commands = { "java", "javac" },
    },
    jdk = {
        apt = "default-jdk",
        dnf = "java-21-openjdk-devel",
        yum = "java-21-openjdk-devel",
        zypper = "java-17-openjdk-devel",
        pacman = "jdk-openjdk",
        apk = "openjdk17",
        emerge = "dev-java/openjdk",
        xbps = "openjdk17",
        eopkg = "openjdk-17",
        urpmi = "java-17-openjdk-devel",
        nix = "jdk",
        brew = "openjdk",
        commands = { "java", "javac" },
    },
    maven = {
        apt = "maven",
        dnf = "maven",
        yum = "maven",
        zypper = "maven",
        pacman = "maven",
        apk = "maven",
        emerge = "dev-java/maven-bin",
        xbps = "maven",
        eopkg = "maven",
        urpmi = "maven",
        nix = "maven",
        brew = "maven",
        commands = { "mvn" },
    },
    gradle = {
        apt = "gradle",
        dnf = "gradle",
        yum = "gradle",
        zypper = "gradle",
        pacman = "gradle",
        apk = "gradle",
        emerge = "dev-java/gradle-bin",
        xbps = "gradle",
        eopkg = "gradle",
        urpmi = "gradle",
        nix = "gradle",
        brew = "gradle",
        commands = { "gradle" },
    },
    python = {
        apt = "python3",
        dnf = "python3",
        yum = "python3",
        zypper = "python3",
        pacman = "python",
        apk = "python3",
        emerge = "dev-lang/python",
        xbps = "python3",
        eopkg = "python3",
        urpmi = "python3",
        nix = "python3",
        brew = "python",
        commands = { "python3" },
    },
    pip = {
        apt = "python3-pip",
        dnf = "python3-pip",
        yum = "python3-pip",
        zypper = "python3-pip",
        pacman = "python-pip",
        apk = "py3-pip",
        emerge = "dev-python/pip",
        xbps = "python3-pip",
        eopkg = "python3-pip",
        urpmi = "python3-pip",
        nix = "python3Packages.pip",
        brew = "python",
        commands = { "pip3" },
    },
    node = {
        apt = "nodejs",
        dnf = "nodejs",
        yum = "nodejs",
        zypper = "nodejs",
        pacman = "nodejs",
        apk = "nodejs",
        emerge = "net-libs/nodejs",
        xbps = "nodejs",
        eopkg = "nodejs",
        urpmi = "nodejs",
        nix = "nodejs",
        brew = "node",
        commands = { "node" },
    },
    npm = {
        apt = "npm",
        dnf = "npm",
        yum = "npm",
        zypper = "npm",
        pacman = "npm",
        apk = "npm",
        emerge = "net-libs/nodejs",
        xbps = "npm",
        eopkg = "npm",
        urpmi = "npm",
        nix = "nodejs",
        brew = "node",
        commands = { "npm" },
    },
    git = {
        apt = "git",
        dnf = "git",
        yum = "git",
        zypper = "git",
        pacman = "git",
        apk = "git",
        emerge = "dev-vcs/git",
        xbps = "git",
        eopkg = "git",
        urpmi = "git",
        nix = "git",
        brew = "git",
        commands = { "git" },
    },
    curl = {
        apt = "curl",
        dnf = "curl",
        yum = "curl",
        zypper = "curl",
        pacman = "curl",
        apk = "curl",
        emerge = "net-misc/curl",
        xbps = "curl",
        eopkg = "curl",
        urpmi = "curl",
        nix = "curl",
        brew = "curl",
        commands = { "curl" },
    },
}

local function normalized_backend_name(value)
    local normalized = trim(value):lower()
    if normalized == "" then
        return nil
    end
    if normalized == "apt" or normalized == "apt-get" or normalized == "debian" or normalized == "ubuntu" then
        return "apt"
    end
    if normalized == "dnf" or normalized == "fedora" or normalized == "rhel" then
        return "dnf"
    end
    if normalized == "yum" then
        return "yum"
    end
    if normalized == "zypper" or normalized == "opensuse" or normalized == "suse" then
        return "zypper"
    end
    if normalized == "pacman" or normalized == "arch" then
        return "pacman"
    end
    if normalized == "apk" or normalized == "alpine" then
        return "apk"
    end
    if normalized == "emerge" or normalized == "gentoo" then
        return "emerge"
    end
    if normalized == "xbps" or normalized == "void" then
        return "xbps"
    end
    if normalized == "eopkg" or normalized == "solus" then
        return "eopkg"
    end
    if normalized == "urpmi" or normalized == "mageia" then
        return "urpmi"
    end
    if normalized == "nix" or normalized == "nix-env" or normalized == "nixos" then
        return "nix"
    end
    if normalized == "brew" or normalized == "homebrew" or normalized == "darwin" or normalized == "macos" then
        return "brew"
    end
    return nil
end

local function backend_binary(backend)
    if backend == "apt" then
        return apt_binary()
    end
    if backend == "dnf" then
        return dnf_binary()
    end
    if backend == "yum" then
        return yum_binary()
    end
    if backend == "zypper" then
        return zypper_binary()
    end
    if backend == "pacman" then
        return pacman_binary()
    end
    if backend == "apk" then
        return apk_binary()
    end
    if backend == "emerge" then
        return emerge_binary()
    end
    if backend == "xbps" then
        return xbps_install_binary()
    end
    if backend == "eopkg" then
        return eopkg_binary()
    end
    if backend == "urpmi" then
        return urpmi_binary()
    end
    if backend == "nix" then
        return nix_binary()
    end
    if backend == "brew" then
        return brew_binary()
    end
    return nil
end

local function detect_backend_from_os_release()
    local os_release = command_stdout("cat /etc/os-release 2>/dev/null")
    local lowered = os_release:lower()
    if lowered:find("fedora", 1, true) ~= nil then
        return "dnf"
    end
    if lowered:find("rhel", 1, true) ~= nil or lowered:find("centos", 1, true) ~= nil or
        lowered:find("rocky", 1, true) ~= nil or lowered:find("alma", 1, true) ~= nil or
        lowered:find("amzn", 1, true) ~= nil then
        if binary_is_available(dnf_binary()) then
            return "dnf"
        end
        if binary_is_available(yum_binary()) then
            return "yum"
        end
        return "dnf"
    end
    if lowered:find("ubuntu", 1, true) ~= nil or lowered:find("debian", 1, true) ~= nil or
        lowered:find("mint", 1, true) ~= nil or lowered:find("pop", 1, true) ~= nil or
        lowered:find("kali", 1, true) ~= nil then
        return "apt"
    end
    if lowered:find("opensuse", 1, true) ~= nil or lowered:find("suse", 1, true) ~= nil or
        lowered:find("sles", 1, true) ~= nil then
        return "zypper"
    end
    if lowered:find("arch", 1, true) ~= nil or lowered:find("manjaro", 1, true) ~= nil or
        lowered:find("endeavour", 1, true) ~= nil or lowered:find("artix", 1, true) ~= nil then
        return "pacman"
    end
    if lowered:find("alpine", 1, true) ~= nil then
        return "apk"
    end
    if lowered:find("gentoo", 1, true) ~= nil or lowered:find("calculate", 1, true) ~= nil then
        return "emerge"
    end
    if lowered:find("void", 1, true) ~= nil then
        return "xbps"
    end
    if lowered:find("solus", 1, true) ~= nil then
        return "eopkg"
    end
    if lowered:find("mageia", 1, true) ~= nil or lowered:find("mandriva", 1, true) ~= nil then
        return "urpmi"
    end
    if lowered:find("nixos", 1, true) ~= nil then
        return "nix"
    end
    local uname = command_stdout("uname -s 2>/dev/null")
    if uname == "Darwin" then
        return "brew"
    end
    return nil
end

local function detect_backend()
    local override = normalized_backend_name(getenv("REQPACK_SYS_BACKEND", ""))
    if override ~= nil then
        return override
    end

    local preferred = detect_backend_from_os_release()
    if preferred ~= nil and binary_is_available(backend_binary(preferred)) then
        return preferred
    end
    if binary_is_available(dnf_binary()) then
        return "dnf"
    end
    if binary_is_available(yum_binary()) then
        return "yum"
    end
    if binary_is_available(apt_binary()) then
        return "apt"
    end
    if binary_is_available(zypper_binary()) then
        return "zypper"
    end
    if binary_is_available(pacman_binary()) then
        return "pacman"
    end
    if binary_is_available(apk_binary()) then
        return "apk"
    end
    if binary_is_available(emerge_binary()) then
        return "emerge"
    end
    if binary_is_available(xbps_install_binary()) then
        return "xbps"
    end
    if binary_is_available(eopkg_binary()) then
        return "eopkg"
    end
    if binary_is_available(urpmi_binary()) then
        return "urpmi"
    end
    if binary_is_available(nix_binary()) then
        return "nix"
    end
    if binary_is_available(brew_binary()) then
        return "brew"
    end
    return preferred
end

local function sudo_prefix(backend)
    if backend == "brew" or backend == "nix" or env_is_true("REQPACK_SYS_NO_SUDO") then
        return ""
    end
    if command_stdout("id -u 2>/dev/null") == "0" then
        return ""
    end
    local sudo = sudo_binary()
    if binary_is_available(sudo) then
        return shell_quote(sudo) .. " "
    end
    return ""
end

local function nix_install_command()
    local override = trim(getenv("REQPACK_SYS_NIX_INSTALL_CMD", ""))
    if override ~= "" then
        return override
    end

    local url = trim(getenv("REQPACK_SYS_NIX_INSTALL_URL", "https://nixos.org/nix/install"))
    local flags = trim(getenv("REQPACK_SYS_NIX_INSTALL_FLAGS", "--yes --no-daemon"))
    local suffix = flags ~= "" and (" " .. flags) or ""

    local curl = curl_binary()
    if binary_is_available(curl) then
        return shell_quote(curl) .. " -fsSL " .. shell_quote(url) .. " | sh -s --" .. suffix
    end

    return nil
end

local function ensure_nix_available(context)
    if binary_is_available(nix_binary()) then
        return true, nil
    end

    if env_is_true("REQPACK_SYS_DISABLE_NIX_BOOTSTRAP") then
        return false, "nix backend required but nix bootstrap is disabled"
    end

    local installer = nix_install_command()
    if installer == nil then
        return false, "nix backend required but no nix installer is available"
    end

    if context ~= nil then
        context.tx.begin_step("bootstrap nix package manager")
    end

    local result = reqpack.exec.run(installer)
    if not result.success then
        return false, "failed to bootstrap nix package manager"
    end

    if not binary_is_available(nix_binary()) then
        return false, "nix bootstrap completed but nix-env is still unavailable"
    end

    return true, nil
end

local function logical_package(name)
    return LOGICAL_PACKAGES[trim(name):lower()]
end

local function resolved_package_name(name, backend)
    local logical = logical_package(name)
    if logical ~= nil and logical[backend] ~= nil and logical[backend] ~= "" then
        return logical[backend]
    end
    return trim(name)
end

local function resolved_package_commands(name)
    local logical = logical_package(name)
    if logical == nil then
        return {}
    end
    return logical.commands or {}
end

local function resolved_package_spec(pkg, backend)
    local name = resolved_package_name(pkg.name or "", backend)
    local version = trim(pkg.version or "")
    if version == "" then
        if backend == "nix" then
            return "nixpkgs." .. name
        end
        return name
    end
    if backend == "apt" then
        return name .. "=" .. version
    end
    if backend == "dnf" or backend == "yum" or backend == "urpmi" then
        return name .. "-" .. version
    end
    if backend == "zypper" then
        return name .. "=" .. version
    end
    if backend == "nix" then
        return "nixpkgs." .. name
    end
    if backend == "brew" then
        return name .. "@" .. version
    end
    return name
end

local function shell_join(values)
    local quoted = {}
    for _, value in ipairs(values or {}) do
        table.insert(quoted, shell_quote(value))
    end
    return table.concat(quoted, " ")
end

local function package_commands_available(name)
    local commands = resolved_package_commands(name)
    if #commands == 0 then
        return nil
    end
    for _, command_name in ipairs(commands) do
        if not binary_is_available(command_name) then
            return false
        end
    end
    return true
end

local function apt_package_installed(name)
    local dpkg_query = dpkg_query_binary()
    if not binary_is_available(dpkg_query) then
        return false
    end
    return command_succeeds(shell_quote(dpkg_query) .. " -W -f='${Status}' " .. shell_quote(name) ..
        " 2>/dev/null | grep -q 'install ok installed'")
end

local function dnf_package_installed(name)
    local rpm = rpm_binary()
    if not binary_is_available(rpm) then
        return false
    end
    return command_succeeds(shell_quote(rpm) .. " -q --quiet " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function pacman_package_installed(name)
    local pacman = pacman_binary()
    if not binary_is_available(pacman) then
        return false
    end
    return command_succeeds(shell_quote(pacman) .. " -Q " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function apk_package_installed(name)
    local apk = apk_binary()
    if not binary_is_available(apk) then
        return false
    end
    return command_succeeds(shell_quote(apk) .. " info -e " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function xbps_package_installed(name)
    local query = xbps_query_binary()
    if not binary_is_available(query) then
        return false
    end
    return command_succeeds(shell_quote(query) .. " " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function eopkg_package_installed(name)
    local eopkg = eopkg_binary()
    if not binary_is_available(eopkg) then
        return false
    end
    return command_succeeds(shell_quote(eopkg) .. " list-installed " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function emerge_package_installed(name)
    local qlist = qlist_binary()
    if binary_is_available(qlist) then
        return command_succeeds(shell_quote(qlist) .. " -ICv " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    local equery = equery_binary()
    if binary_is_available(equery) then
        return command_succeeds(shell_quote(equery) .. " list " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    return nil
end

local function brew_package_installed(name)
    local brew = brew_binary()
    if not binary_is_available(brew) then
        return false
    end
    return command_succeeds(shell_quote(brew) .. " list --formula " .. shell_quote(name) .. " >/dev/null 2>&1") or
        command_succeeds(shell_quote(brew) .. " list --cask " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function nix_package_installed(name)
    local nix = nix_binary()
    if not binary_is_available(nix) then
        return false
    end
    return command_succeeds(shell_quote(nix) .. " -q " .. shell_quote(name) .. " >/dev/null 2>&1")
end

local function package_query_installed(pkg, backend)
    local name = resolved_package_name(pkg.name or "", backend)
    if backend == "apt" then
        local dpkg_query = dpkg_query_binary()
        if not binary_is_available(dpkg_query) then
            return nil
        end
        return apt_package_installed(name)
    end
    if backend == "dnf" or backend == "yum" or backend == "zypper" or backend == "urpmi" then
        local rpm = rpm_binary()
        if not binary_is_available(rpm) then
            return nil
        end
        return dnf_package_installed(name)
    end
    if backend == "pacman" then
        local pacman = pacman_binary()
        if not binary_is_available(pacman) then
            return nil
        end
        return pacman_package_installed(name)
    end
    if backend == "apk" then
        local apk = apk_binary()
        if not binary_is_available(apk) then
            return nil
        end
        return apk_package_installed(name)
    end
    if backend == "emerge" then
        return emerge_package_installed(name)
    end
    if backend == "xbps" then
        local query = xbps_query_binary()
        if not binary_is_available(query) then
            return nil
        end
        return xbps_package_installed(name)
    end
    if backend == "eopkg" then
        local eopkg = eopkg_binary()
        if not binary_is_available(eopkg) then
            return nil
        end
        return eopkg_package_installed(name)
    end
    if backend == "nix" then
        local nix = nix_binary()
        if not binary_is_available(nix) then
            return nil
        end
        return nix_package_installed(name)
    end
    if backend == "brew" then
        local brew = brew_binary()
        if not binary_is_available(brew) then
            return nil
        end
        return brew_package_installed(name)
    end
    return nil
end

local function package_installed(pkg, backend)
    local query_state = package_query_installed(pkg, backend)
    if query_state == true then
        return true
    end

    if backend ~= "nix" then
        local nix_state = package_query_installed(pkg, "nix")
        if nix_state == true then
            return true
        end
    end

    if query_state ~= nil then
        return query_state
    end

    local command_state = package_commands_available(pkg.name or "")
    if command_state ~= nil then
        return command_state
    end
    return false
end

local function backend_available(context)
    local backend = detect_backend()
    if backend == nil then
        local ensured, nix_error = ensure_nix_available(context)
        if ensured then
            return "nix", nil
        end
        return nil, nix_error or "no supported system package manager found"
    end
    local binary = backend_binary(backend)
    if not binary_is_available(binary) then
        if backend == "nix" then
            local ensured, nix_error = ensure_nix_available(context)
            if ensured then
                return "nix", nil
            end
            return nil, nix_error or "system package manager binary not found for backend 'nix'"
        end
        return nil, "system package manager binary not found for backend '" .. backend .. "'"
    end
    return backend, nil
end

local function package_available_in_backend(pkg, backend)
    local name = resolved_package_name(pkg.name or "", backend)
    if name == "" then
        return false
    end
    if backend == "apt" then
        local apt_cache = apt_cache_binary()
        if not binary_is_available(apt_cache) then
            return nil
        end
        return command_succeeds(shell_quote(apt_cache) .. " show " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "dnf" or backend == "yum" then
        local binary = backend_binary(backend)
        if not binary_is_available(binary) then
            return nil
        end
        return command_succeeds(shell_quote(binary) .. " info " .. shell_quote(name) .. " --quiet >/dev/null 2>&1")
    end
    if backend == "zypper" then
        local zypper = zypper_binary()
        if not binary_is_available(zypper) then
            return nil
        end
        return command_succeeds(shell_quote(zypper) .. " info " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "pacman" then
        local pacman = pacman_binary()
        if not binary_is_available(pacman) then
            return nil
        end
        return command_succeeds(shell_quote(pacman) .. " -Si " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "apk" then
        local apk = apk_binary()
        if not binary_is_available(apk) then
            return nil
        end
        return command_succeeds(shell_quote(apk) .. " search " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "emerge" then
        local emerge = emerge_binary()
        if not binary_is_available(emerge) then
            return nil
        end
        return command_succeeds(shell_quote(emerge) .. " --search " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "xbps" then
        local query = xbps_query_binary()
        if not binary_is_available(query) then
            return nil
        end
        return command_succeeds(shell_quote(query) .. " -Rs " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "eopkg" then
        local eopkg = eopkg_binary()
        if not binary_is_available(eopkg) then
            return nil
        end
        return command_succeeds(shell_quote(eopkg) .. " search " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "urpmi" then
        local urpmi = urpmi_binary()
        if not binary_is_available(urpmi) then
            return nil
        end
        return command_succeeds(shell_quote(urpmi) .. " --searchmedia " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    if backend == "nix" then
        local nix = nix_binary()
        if not binary_is_available(nix) then
            return nil
        end
        return command_succeeds(shell_quote(nix) .. " -qaA " .. shell_quote("nixpkgs." .. name) .. " >/dev/null 2>&1")
    end
    if backend == "brew" then
        local brew = brew_binary()
        if not binary_is_available(brew) then
            return nil
        end
        return command_succeeds(shell_quote(brew) .. " info --formula " .. shell_quote(name) .. " >/dev/null 2>&1") or
            command_succeeds(shell_quote(brew) .. " info --cask " .. shell_quote(name) .. " >/dev/null 2>&1")
    end
    return nil
end

local function partition_packages_for_install(context, packages, backend)
    if backend == "nix" then
        local ensured, nix_error = ensure_nix_available(context)
        if not ensured then
            return nil, nil, nix_error
        end
        return packages or {}, {}, nil
    end

    local backend_packages = {}
    local nix_packages = {}
    for _, pkg in ipairs(packages or {}) do
        local available = package_available_in_backend(pkg, backend)
        if available ~= false then
            table.insert(backend_packages, pkg)
        else
            local ensured, nix_error = ensure_nix_available(context)
            if ensured and package_available_in_backend(pkg, "nix") == true then
                table.insert(nix_packages, pkg)
            else
                table.insert(backend_packages, pkg)
            end
        end
    end
    return backend_packages, nix_packages, nil
end

local function install_command(backend, specs)
    local binary = shell_quote(backend_binary(backend))
    local joined = shell_join(specs)
    if backend == "apt" then
        return sudo_prefix(backend) .. binary .. " update && " .. sudo_prefix(backend) .. binary .. " install -y " .. joined
    end
    if backend == "dnf" then
        return sudo_prefix(backend) .. binary .. " install -y " .. joined
    end
    if backend == "yum" then
        return sudo_prefix(backend) .. binary .. " install -y " .. joined
    end
    if backend == "zypper" then
        return sudo_prefix(backend) .. binary .. " --non-interactive install --auto-agree-with-licenses " .. joined
    end
    if backend == "pacman" then
        return sudo_prefix(backend) .. binary .. " -S --noconfirm --needed " .. joined
    end
    if backend == "apk" then
        return sudo_prefix(backend) .. binary .. " add " .. joined
    end
    if backend == "emerge" then
        return sudo_prefix(backend) .. binary .. " --ask=n " .. joined
    end
    if backend == "xbps" then
        return sudo_prefix(backend) .. binary .. " -Sy " .. joined
    end
    if backend == "eopkg" then
        return sudo_prefix(backend) .. binary .. " install -y " .. joined
    end
    if backend == "urpmi" then
        return sudo_prefix(backend) .. binary .. " --auto " .. joined
    end
    if backend == "nix" then
        return binary .. " -iA " .. joined
    end
    return binary .. " install " .. joined
end

local function remove_command(backend, specs)
    local binary = shell_quote(backend_binary(backend))
    local joined = shell_join(specs)
    if backend == "apt" then
        return sudo_prefix(backend) .. binary .. " remove -y " .. joined
    end
    if backend == "dnf" or backend == "yum" then
        return sudo_prefix(backend) .. binary .. " remove -y " .. joined
    end
    if backend == "zypper" then
        return sudo_prefix(backend) .. binary .. " --non-interactive remove " .. joined
    end
    if backend == "pacman" then
        return sudo_prefix(backend) .. binary .. " -R --noconfirm " .. joined
    end
    if backend == "apk" then
        return sudo_prefix(backend) .. binary .. " del " .. joined
    end
    if backend == "emerge" then
        return sudo_prefix(backend) .. binary .. " --ask=n --depclean " .. joined
    end
    if backend == "xbps" then
        return sudo_prefix(backend) .. shell_quote(xbps_remove_binary()) .. " -Ry " .. joined
    end
    if backend == "eopkg" then
        return sudo_prefix(backend) .. binary .. " remove -y " .. joined
    end
    if backend == "urpmi" then
        return sudo_prefix(backend) .. shell_quote(urpme_binary()) .. " --auto " .. joined
    end
    if backend == "nix" then
        return binary .. " -e " .. joined
    end
    return binary .. " uninstall " .. joined
end

local function update_command(backend, specs)
    local binary = shell_quote(backend_binary(backend))
    if backend == "apt" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " update && " .. sudo_prefix(backend) .. binary .. " upgrade -y"
        end
        return sudo_prefix(backend) .. binary .. " update && " .. sudo_prefix(backend) .. binary .. " install --only-upgrade -y " .. shell_join(specs)
    end
    if backend == "dnf" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " upgrade -y"
        end
        return sudo_prefix(backend) .. binary .. " upgrade -y " .. shell_join(specs)
    end
    if backend == "yum" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " update -y"
        end
        return sudo_prefix(backend) .. binary .. " update -y " .. shell_join(specs)
    end
    if backend == "zypper" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " --non-interactive update"
        end
        return sudo_prefix(backend) .. binary .. " --non-interactive update " .. shell_join(specs)
    end
    if backend == "pacman" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " -Syu --noconfirm"
        end
        return sudo_prefix(backend) .. binary .. " -S --noconfirm " .. shell_join(specs)
    end
    if backend == "apk" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " update && " .. sudo_prefix(backend) .. binary .. " upgrade"
        end
        return sudo_prefix(backend) .. binary .. " add --upgrade " .. shell_join(specs)
    end
    if backend == "emerge" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " --ask=n --update --deep --newuse @world"
        end
        return sudo_prefix(backend) .. binary .. " --ask=n --update --deep --newuse " .. shell_join(specs)
    end
    if backend == "xbps" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " -Syu"
        end
        return sudo_prefix(backend) .. binary .. " -Syu " .. shell_join(specs)
    end
    if backend == "eopkg" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " upgrade -y"
        end
        return sudo_prefix(backend) .. binary .. " upgrade -y " .. shell_join(specs)
    end
    if backend == "urpmi" then
        if specs == nil or #specs == 0 then
            return sudo_prefix(backend) .. binary .. " --auto-select --auto"
        end
        return sudo_prefix(backend) .. binary .. " --auto " .. shell_join(specs)
    end
    if backend == "nix" then
        if specs == nil or #specs == 0 then
            return binary .. " --upgrade"
        end
        return binary .. " -iA " .. shell_join(specs)
    end
    if specs == nil or #specs == 0 then
        return binary .. " upgrade"
    end
    return binary .. " upgrade " .. shell_join(specs)
end

local function resolve_specs(packages, backend)
    local specs = {}
    for _, pkg in ipairs(packages or {}) do
        table.insert(specs, resolved_package_spec(pkg, backend))
    end
    return specs
end

function plugin.getName()
    return "ReqPack System Provisioning Manager"
end

function plugin.getVersion()
    return "1.2.0"
end

function plugin.getCategories()
    return { "System", "Provisioning", "Proxy" }
end

plugin.fileExtensions = { ".deb", ".rpm", ".apk", ".xbps", ".eopkg", ".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz" }

function plugin.getRequirements()
    return {}
end

function plugin.getMissingPackages(packages)
    local backend = detect_backend()
    if backend == nil then
        local missing = {}
        for _, pkg in ipairs(packages or {}) do
            table.insert(missing, pkg)
        end
        return missing
    end

    local missing = {}
    for _, pkg in ipairs(packages or {}) do
        local action = pkg.action
        local installed = package_installed(pkg, backend)
        if action == "remove" or action == 2 then
            if installed then
                table.insert(missing, pkg)
            end
        elseif action == "update" or action == 3 then
            if installed then
                table.insert(missing, pkg)
            elseif trim(pkg.version or "") ~= "" then
                table.insert(missing, pkg)
            end
        elseif not installed then
            table.insert(missing, pkg)
        end
    end
    return missing
end

function plugin.install(context, packages)
    if #packages == 0 then return true end

    local backend, error_message = backend_available(context)
    if backend == nil then
        context.tx.failed(error_message)
        return false
    end

    local backend_packages, nix_packages, partition_error = partition_packages_for_install(context, packages, backend)
    if partition_error ~= nil then
        context.tx.failed(partition_error)
        return false
    end

    if #backend_packages > 0 then
        local specs = resolve_specs(backend_packages, backend)
        context.tx.begin_step("install system packages via " .. backend)
        local result = context.exec.run(install_command(backend, specs))
        if not result.success then
            context.tx.failed("sys install failed via " .. backend)
            return false
        end
        context.events.installed(specs)
    end

    if #nix_packages > 0 then
        local nix_specs = resolve_specs(nix_packages, "nix")
        context.tx.begin_step("install fallback packages via nix")
        local nix_result = context.exec.run(install_command("nix", nix_specs))
        if not nix_result.success then
            context.tx.failed("sys install failed via nix fallback")
            return false
        end
        context.events.installed(nix_specs)
    end

    context.tx.success()
    return true
end

function plugin.installLocal(context, path)
    local backend, error_message = backend_available(context)
    if backend == nil then
        context.tx.failed(error_message)
        return false
    end

    context.tx.begin_step("install local system package via " .. backend)
    local binary = shell_quote(backend_binary(backend))
    local command = ""
    if backend == "apt" then
        command = sudo_prefix(backend) .. binary .. " install -y " .. shell_quote(path)
    elseif backend == "dnf" or backend == "yum" or backend == "zypper" or backend == "urpmi" then
        command = sudo_prefix(backend) .. binary .. " install -y " .. shell_quote(path)
    elseif backend == "pacman" then
        command = sudo_prefix(backend) .. binary .. " -U --noconfirm " .. shell_quote(path)
    elseif backend == "apk" then
        command = sudo_prefix(backend) .. binary .. " add --allow-untrusted " .. shell_quote(path)
    elseif backend == "xbps" then
        command = sudo_prefix(backend) .. binary .. " -y " .. shell_quote(path)
    elseif backend == "eopkg" then
        command = sudo_prefix(backend) .. binary .. " install -y " .. shell_quote(path)
    elseif backend == "emerge" then
        context.tx.failed("sys local install not supported via emerge")
        return false
    elseif backend == "nix" then
        command = binary .. " -i " .. shell_quote(path)
    else
        command = binary .. " install " .. shell_quote(path)
    end
    local result = context.exec.run(command)
    if not result.success then
        context.tx.failed("sys local install failed via " .. backend)
        return false
    end
    context.events.installed({ path = path, localTarget = true })
    context.tx.success()
    return true
end

function plugin.remove(context, packages)
    if #packages == 0 then return true end

    local backend, error_message = backend_available(context)
    if backend == nil then
        context.tx.failed(error_message)
        return false
    end

    local specs = resolve_specs(packages, backend)
    context.tx.begin_step("remove system packages via " .. backend)
    local result = context.exec.run(remove_command(backend, specs))
    if not result.success then
        context.tx.failed("sys remove failed via " .. backend)
        return false
    end
    context.events.deleted(specs)
    context.tx.success()
    return true
end

function plugin.update(context, packages)
    local backend, error_message = backend_available(context)
    if backend == nil then
        context.tx.failed(error_message)
        return false
    end

    local specs = resolve_specs(packages or {}, backend)
    context.tx.begin_step("update system packages via " .. backend)
    local result = context.exec.run(update_command(backend, specs))
    if not result.success then
        context.tx.failed("sys update failed via " .. backend)
        return false
    end
    context.events.updated(specs)
    context.tx.success()
    return true
end

function plugin.list(context)
    local backend = detect_backend()
    if backend == nil then
        return {}
    end

    local items = {}
    if backend == "apt" then
        local dpkg_query = dpkg_query_binary()
        if not binary_is_available(dpkg_query) then
            return items
        end
        local result = context.exec.run(shell_quote(dpkg_query) .. " -W -f='${Package}\t${Version}\n'")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, version = line:match("^(.-)\t(.+)$")
            if name and version then
                table.insert(items, { name = name, version = version, description = "Installed apt package" })
            end
        end
    elseif backend == "dnf" or backend == "yum" or backend == "zypper" or backend == "urpmi" then
        local rpm = rpm_binary()
        if not binary_is_available(rpm) then
            return items
        end
        local result = context.exec.run(shell_quote(rpm) .. " -qa --qf '%{NAME}\t%{VERSION}-%{RELEASE}\n'")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, version = line:match("^(.-)\t(.+)$")
            if name and version then
                table.insert(items, { name = name, version = version, description = "Installed system package" })
            end
        end
    elseif backend == "pacman" then
        local result = context.exec.run(shell_quote(pacman_binary()) .. " -Q")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, version = line:match("^(%S+)%s+(.+)$")
            if name and version then
                table.insert(items, { name = name, version = version, description = "Installed pacman package" })
            end
        end
    elseif backend == "apk" then
        local result = context.exec.run(shell_quote(apk_binary()) .. " info -v")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "installed", description = "Installed apk package" })
            end
        end
    elseif backend == "emerge" then
        local qlist = qlist_binary()
        local result
        if binary_is_available(qlist) then
            result = context.exec.run(shell_quote(qlist) .. " -ICv")
        else
            local equery = equery_binary()
            if not binary_is_available(equery) then
                context.events.listed(items)
                return items
            end
            result = context.exec.run(shell_quote(equery) .. " list '*'")
        end
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "installed", description = "Installed emerge package" })
            end
        end
    elseif backend == "xbps" then
        local result = context.exec.run(shell_quote(xbps_query_binary()) .. " -l")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local package_name = line:match("^ii%s+([^%s]+)") or line:match("^uu%s+([^%s]+)")
            if package_name then
                table.insert(items, { name = package_name, version = "installed", description = "Installed xbps package" })
            end
        end
    elseif backend == "eopkg" then
        local result = context.exec.run(shell_quote(eopkg_binary()) .. " list-installed")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name and name ~= "Installed" then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "nix" then
        local result = context.exec.run(shell_quote(nix_binary()) .. " -q")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "installed", description = "Installed nix package" })
            end
        end
    else
        local result = context.exec.run(shell_quote(brew_binary()) .. " list --versions")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, version = line:match("^(%S+)%s+(.+)$")
            if name and version then
                table.insert(items, { name = name, version = version, description = "Installed brew package" })
            end
        end
    end
    context.events.listed(items)
    return items
end

function plugin.outdated(context)
    local backend = detect_backend()
    if backend == nil then
        return {}
    end

    local items = {}
    if backend == "apt" then
        local result = context.exec.run(shell_quote(apt_list_binary()) .. " list --upgradable 2>/dev/null")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, version = line:match("^(.-)/.-%s+([^%s]+)")
            if name and version then
                table.insert(items, { name = name, version = version, description = "Update available" })
            end
        end
    elseif backend == "dnf" or backend == "yum" then
        local result = context.exec.run(shell_quote(backend_binary(backend)) .. " check-update --quiet 2>/dev/null; echo \"EXIT:$?\"")
        local stdout = result.stdout or ""
        for line in stdout:gmatch("[^\r\n]+") do
            if line:match("^EXIT:") then break end
            local name, version = line:match("^(%S+)%s+(%S+)%s")
            if name and version then
                local base_name = name:match("^(.-)%.[^.]+$") or name
                table.insert(items, { name = base_name, version = version, description = "Update available" })
            end
        end
    elseif backend == "zypper" then
        local result = context.exec.run(shell_quote(zypper_binary()) .. " --non-interactive list-updates")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("|%s*([^|%s]+)%s*|")
            if name and name ~= "Repository" then
                table.insert(items, { name = trim(name), version = "installed", description = trim(line) })
            end
        end
    elseif backend == "pacman" then
        local result = context.exec.run(shell_quote(pacman_binary()) .. " -Qu")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "apk" then
        local result = context.exec.run(shell_quote(apk_binary()) .. " version -l '<'")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "emerge" then
        local result = context.exec.run(shell_quote(emerge_binary()) .. " -puDN @world 2>/dev/null")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            if line:find("[ebuild", 1, true) ~= nil then
                table.insert(items, { name = trim(line), version = "installed", description = "Update available" })
            end
        end
    elseif backend == "xbps" then
        local result = context.exec.run(shell_quote(xbps_install_binary()) .. " -un")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "eopkg" then
        local result = context.exec.run(shell_quote(eopkg_binary()) .. " list-upgrades")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name and name ~= "Upgradable" then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "urpmi" then
        local result = context.exec.run(shell_quote(urpmi_binary()) .. " --auto-select --auto --test")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    elseif backend == "nix" then
        local result = context.exec.run(shell_quote(nix_binary()) .. " -u '*' --dry-run 2>/dev/null")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("'([^']+)'")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    else
        local result = context.exec.run(shell_quote(brew_binary()) .. " outdated --verbose 2>/dev/null")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name then
                table.insert(items, { name = name, version = "installed", description = trim(line) })
            end
        end
    end
    context.events.outdated(items)
    return items
end

function plugin.search(context, prompt)
    local backend = detect_backend()
    if backend == nil then
        return {}
    end

    local items = {}
    if backend == "apt" then
        local result = context.exec.run(shell_quote(apt_cache_binary()) .. " search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name, description = line:match("^(.-)%s+%-%s+(.+)$")
            if name and description then
                table.insert(items, { name = trim(name), version = "repo", description = trim(description) })
            end
        end
    elseif backend == "dnf" or backend == "yum" then
        local result = context.exec.run(shell_quote(backend_binary(backend)) .. " search " .. shell_quote(prompt) .. " --quiet")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name ~= nil and name ~= "Last" and name ~= "Matched" then
                table.insert(items, { name = name, version = "repo", description = trim(line) })
            end
        end
    elseif backend == "zypper" then
        local result = context.exec.run(shell_quote(zypper_binary()) .. " search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("|%s*([^|%s]+)%s*|")
            if name and name ~= "Name" then
                table.insert(items, { name = trim(name), version = "repo", description = trim(line) })
            end
        end
    elseif backend == "pacman" then
        local result = context.exec.run(shell_quote(pacman_binary()) .. " -Ss " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^[^/]+/([^%s]+)")
            if name then
                table.insert(items, { name = name, version = "repo", description = trim(line) })
            end
        end
    elseif backend == "apk" then
        local result = context.exec.run(shell_quote(apk_binary()) .. " search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "repo", description = "Available via apk" })
            end
        end
    elseif backend == "emerge" then
        local result = context.exec.run(shell_quote(emerge_binary()) .. " --search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" and name:find("Searching", 1, true) == nil then
                table.insert(items, { name = name, version = "repo", description = "Available via emerge" })
            end
        end
    elseif backend == "xbps" then
        local result = context.exec.run(shell_quote(xbps_query_binary()) .. " -Rs " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^- ([^%s]+)") or line:match("^ii ([^%s]+)")
            if name then
                table.insert(items, { name = name, version = "repo", description = trim(line) })
            end
        end
    elseif backend == "eopkg" then
        local result = context.exec.run(shell_quote(eopkg_binary()) .. " search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = line:match("^(%S+)")
            if name and name ~= "Package" then
                table.insert(items, { name = name, version = "repo", description = trim(line) })
            end
        end
    elseif backend == "urpmi" then
        local result = context.exec.run(shell_quote(urpmi_binary()) .. " --searchmedia " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "repo", description = "Available via urpmi" })
            end
        end
    elseif backend == "nix" then
        local result = context.exec.run(shell_quote(nix_binary()) .. " -qaP " .. shell_quote(prompt) .. " 2>/dev/null")
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local attribute, name = line:match("^(%S+)%s+([^%s]+)$")
            if attribute and name then
                table.insert(items, { name = attribute, version = name, description = "Available via Nix" })
            end
        end
    else
        local result = context.exec.run(shell_quote(brew_binary()) .. " search " .. shell_quote(prompt))
        for line in (result.stdout or ""):gmatch("[^\r\n]+") do
            local name = trim(line)
            if name ~= "" then
                table.insert(items, { name = name, version = "repo", description = "Available via Homebrew" })
            end
        end
    end
    context.events.searched(items)
    return items
end

function plugin.info(context, name)
    local backend = detect_backend()
    if backend == nil then
        return { name = name, version = "unknown", description = "No supported system package manager found" }
    end

    local resolved = resolved_package_name(name, backend)
    local result
    if backend == "apt" then
        result = context.exec.run(shell_quote(apt_cache_binary()) .. " show " .. shell_quote(resolved) .. " 2>/dev/null")
    elseif backend == "dnf" or backend == "yum" then
        result = context.exec.run(shell_quote(backend_binary(backend)) .. " info " .. shell_quote(resolved) .. " --quiet")
    elseif backend == "zypper" then
        result = context.exec.run(shell_quote(zypper_binary()) .. " info " .. shell_quote(resolved))
    elseif backend == "pacman" then
        result = context.exec.run(shell_quote(pacman_binary()) .. " -Si " .. shell_quote(resolved))
    elseif backend == "apk" then
        result = context.exec.run(shell_quote(apk_binary()) .. " info -a " .. shell_quote(resolved))
    elseif backend == "emerge" then
        local equery = equery_binary()
        if binary_is_available(equery) then
            result = context.exec.run(shell_quote(equery) .. " meta " .. shell_quote(resolved))
        else
            result = context.exec.run(shell_quote(emerge_binary()) .. " --info " .. shell_quote(resolved))
        end
    elseif backend == "xbps" then
        result = context.exec.run(shell_quote(xbps_query_binary()) .. " -RS " .. shell_quote(resolved))
    elseif backend == "eopkg" then
        result = context.exec.run(shell_quote(eopkg_binary()) .. " info " .. shell_quote(resolved))
    elseif backend == "urpmi" then
        result = context.exec.run(shell_quote(urpmi_binary()) .. " --test " .. shell_quote(resolved))
    elseif backend == "nix" then
        result = context.exec.run(shell_quote(nix_binary()) .. " -qaP --description " .. shell_quote(resolved) .. " 2>/dev/null")
    else
        result = context.exec.run(shell_quote(brew_binary()) .. " info --formula " .. shell_quote(resolved) .. " 2>/dev/null")
    end
    local description = trim((result and result.stdout) or "")
    local item = {
        name = resolved,
        version = package_installed({ name = name }, backend) and "installed" or "repo",
        description = description ~= "" and description or ("System package via " .. backend)
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
