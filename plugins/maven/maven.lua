plugin = {
    name = "Apache Maven Manager",
    version = "1.1.0",
    author = "OpenCode",
    description = "Maven plugin with Java and Maven prerequisites via sys"
}

local function trim(value)
    return (value:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function getenv(name, fallback)
    local value = os.getenv(name)
    if value == nil or trim(value) == "" then
        return fallback
    end

    return trim(value)
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

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function sys_call(cmd)
    print("[Maven-Exec] " .. cmd)
    local success = os.execute(cmd)
    return success == true or success == 0
end

local function split(value, separator)
    local parts = {}
    for part in tostring(value):gmatch("[^" .. separator .. "]+") do
        table.insert(parts, part)
    end
    return parts
end

local function join(parts, separator)
    return table.concat(parts, separator)
end

local function path_exists(path)
    local handle = io.open(path, "r")
    if handle ~= nil then
        handle:close()
        return true
    end

    local probe = io.popen("test -d " .. shell_quote(path) .. " >/dev/null 2>&1 && printf 1")
    if probe == nil then
        return false
    end

    local result = probe:read("*a") or ""
    probe:close()
    return trim(result) == "1"
end

local function maven_repo_dir()
    return getenv("REQPACK_MAVEN_REPO", getenv("HOME", ".") .. "/.m2/repository")
end

local function artifact_from_pkg(pkg)
    local raw = trim(pkg.name or "")
    local version = trim(pkg.version or "")
    local parts = split(raw, ":")

    if #parts < 2 then
        return nil, "Artifact must be 'groupId:artifactId[:packaging[:classifier]][:version]'"
    end

    local artifact = {
        groupId = parts[1],
        artifactId = parts[2],
        packaging = "jar",
        classifier = nil,
        version = version ~= "" and version or nil
    }

    if #parts == 3 then
        artifact.version = artifact.version or parts[3]
    elseif #parts == 4 then
        artifact.packaging = parts[3]
        artifact.version = artifact.version or parts[4]
    elseif #parts >= 5 then
        artifact.packaging = parts[3]
        artifact.classifier = parts[4]
        artifact.version = artifact.version or parts[5]
    end

    if artifact.version == nil or trim(artifact.version) == "" then
        return nil, "Artifact version is required"
    end

    return artifact, nil
end

local function artifact_coordinate(artifact)
    local parts = { artifact.groupId, artifact.artifactId }
    if artifact.packaging ~= nil and artifact.packaging ~= "" and (artifact.packaging ~= "jar" or artifact.classifier ~= nil) then
        table.insert(parts, artifact.packaging)
    end
    if artifact.classifier ~= nil and artifact.classifier ~= "" then
        if #parts == 2 then
            table.insert(parts, artifact.packaging or "jar")
        end
        table.insert(parts, artifact.classifier)
    end
    table.insert(parts, artifact.version)
    return join(parts, ":")
end

local function artifact_repo_dir(artifact)
    local groupPath = artifact.groupId:gsub("%.", "/")
    return maven_repo_dir() .. "/" .. groupPath .. "/" .. artifact.artifactId .. "/" .. artifact.version
end

local function artifact_filename(artifact)
    local base = artifact.artifactId .. "-" .. artifact.version
    if artifact.classifier ~= nil and artifact.classifier ~= "" then
        base = base .. "-" .. artifact.classifier
    end
    return base .. "." .. (artifact.packaging or "jar")
end

local function artifact_repo_path(artifact)
    return artifact_repo_dir(artifact) .. "/" .. artifact_filename(artifact)
end

local function artifact_display_name(artifact)
    return artifact.groupId .. ":" .. artifact.artifactId
end

local function local_artifact_versions(groupId, artifactId)
    local path = maven_repo_dir() .. "/" .. groupId:gsub("%.", "/") .. "/" .. artifactId
    local handle = io.popen("ls -1 " .. shell_quote(path) .. " 2>/dev/null")
    if handle == nil then
        return {}
    end

    local versions = {}
    for line in handle:lines() do
        local version = trim(line)
        if version ~= "" then
            table.insert(versions, version)
        end
    end
    handle:close()
    table.sort(versions)
    return versions
end

local function search_local_artifacts(prompt)
    local normalized = trim(prompt):lower()

    local repo = maven_repo_dir()
    local handle = io.popen("find " .. shell_quote(repo) .. " -name '*.pom' 2>/dev/null")
    if handle == nil then
        return {}
    end

    local results = {}
    for line in handle:lines() do
        local path = trim(line)
        local relative = path:gsub("^" .. repo .. "/?", "")
        local parts = split(relative, "/")
        if #parts >= 4 then
            local version = parts[#parts - 1]
            local artifactId = parts[#parts - 2]
            local groupId = join({ table.unpack(parts, 1, #parts - 3) }, ".")
            local name = groupId .. ":" .. artifactId
            if normalized == "" or name:lower():find(normalized, 1, true) ~= nil then
                table.insert(results, {
                    name = name,
                    version = version,
                    description = "Installed in local Maven repository"
                })
            end
        end
    end
    handle:close()

    return results
end

local function artifact_query_from_name(name)
    local parts = split(trim(name or ""), ":")
    if #parts < 2 then
        return nil, "Artifact must be 'groupId:artifactId' or include a version"
    end

    local artifact = {
        groupId = parts[1],
        artifactId = parts[2],
        packaging = "jar",
        classifier = nil,
        version = nil
    }

    if #parts == 3 then
        artifact.version = parts[3]
    elseif #parts == 4 then
        artifact.packaging = parts[3]
        artifact.version = parts[4]
    elseif #parts >= 5 then
        artifact.packaging = parts[3]
        artifact.classifier = parts[4]
        artifact.version = parts[5]
    end

    return artifact, nil
end

function plugin.init()
    if not command_exists("java") then
        print("[Lua: Maven] Fehler: java wurde nicht gefunden.")
        return false
    end

    if not command_exists("mvn") then
        print("[Lua: Maven] Fehler: mvn wurde nicht gefunden.")
        return false
    end

    return true
end

function plugin.getCategories()
    return { "Java", "Build", "Maven" }
end

function plugin.getRequirements()
    return {
        { system = "sys", name = getenv("REQPACK_MAVEN_JAVA_PACKAGE", "java-21-openjdk-devel") },
        { system = "sys", name = getenv("REQPACK_MAVEN_PACKAGE", "maven") }
    }
end

function plugin.install(packages)
    if #packages == 0 then return true end

    local commands = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            print("[Lua: Maven] Fehler: " .. err)
            return false
        end

        table.insert(commands, "mvn -q dependency:get -Dtransitive=true -Dartifact=" .. shell_quote(artifact_coordinate(artifact)))
    end

    return sys_call(table.concat(commands, " && "))
end

function plugin.remove(packages)
    if #packages == 0 then return true end

    local paths = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            print("[Lua: Maven] Fehler: " .. err)
            return false
        end

        table.insert(paths, shell_quote(artifact_repo_dir(artifact)))
    end

    return sys_call("rm -rf " .. table.concat(paths, " "))
end

function plugin.search(prompt)
    local results = search_local_artifacts(prompt)
    if #results > 0 then
        return results
    end

    return {
        {
            name = prompt,
            version = "unknown",
            description = "No local Maven artifacts matched"
        }
    }
end

function plugin.update(packages)
    if #packages == 0 then
        return true
    end

    local commands = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            print("[Lua: Maven] Fehler: " .. err)
            return false
        end

        table.insert(commands, "mvn -q dependency:get -U -Dtransitive=true -Dartifact=" .. shell_quote(artifact_coordinate(artifact)))
    end

    return sys_call(table.concat(commands, " && "))
end

function plugin.list()
    return search_local_artifacts("")
end

function plugin.shutdown()
    return true
end

function plugin.info(name)
    local artifact, err = artifact_query_from_name(name)
    if artifact == nil then
        return {
            name = name,
            version = "unknown",
            description = err or "Invalid Maven artifact coordinate"
        }
    end

    local versions = local_artifact_versions(artifact.groupId, artifact.artifactId)
    local latestVersion = #versions > 0 and versions[#versions] or artifact.version or "unknown"
    local resolved = {
        groupId = artifact.groupId,
        artifactId = artifact.artifactId,
        packaging = artifact.packaging,
        classifier = artifact.classifier,
        version = latestVersion
    }
    local repoPath = artifact_repo_path(resolved)

    return {
        name = artifact_display_name(artifact),
        version = latestVersion,
        description = path_exists(repoPath) and ("Installed locally at " .. repoPath) or "Not present in local Maven repository"
    }
end
