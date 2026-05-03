plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
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

local function getenv(name, fallback)
    local result = reqpack.exec.run("printf '%s' \"${" .. name .. ":-}\"")
    local value = trim(result.stdout or "")
    if value == "" then
        return fallback
    end
    return value
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

local function path_exists(path)
    return reqpack.exec.run("test -e " .. shell_quote(path)).success
end

local function resolve_local_file(path)
    local file_type = reqpack.exec.run("test -d " .. shell_quote(path) .. " && printf dir || printf file")
    if not file_type.success or trim(file_type.stdout or "") ~= "dir" then
        return path, nil
    end

    local result = reqpack.exec.run(
        "find " .. shell_quote(path) .. " -type f \\( -name '*.jar' -o -name '*.war' -o -name '*.ear' \\) 2>/dev/null | sort"
    )
    if not result.success then
        return nil, "failed to inspect extracted local artifact"
    end

    local matches = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        if trim(line) ~= "" then
            table.insert(matches, trim(line))
        end
    end

    if #matches == 0 then
        return nil, "no local maven artifact found in extracted archive"
    end
    if #matches > 1 then
        return nil, "multiple local maven artifacts found in extracted archive"
    end
    return matches[1], nil
end

local function url_encode(s)
    return (tostring(s or ""):gsub("[^%w%-%.%_%~]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

local function search_maven_central(context, prompt)
    local normalized = trim(prompt)
    if normalized == "" then return {} end
    local url = "https://search.maven.org/solrsearch/select?q=" .. url_encode(normalized) ..
                "&rows=20&wt=json"
    local result = context.exec.run("curl -sf --max-time 15 " .. shell_quote(url))
    if not result.success then return {} end
    local items = {}
    -- Parse JSON docs array: each entry looks like {"id":"g:a","g":"..","a":"..","latestVersion":".."}
    for entry in (result.stdout or ""):gmatch("{[^{}]+}") do
        local g   = entry:match('"g":"([^"]+)"')
        local a   = entry:match('"a":"([^"]+)"')
        local ver = entry:match('"latestVersion":"([^"]+)"')
        if g and a and ver then
            table.insert(items, {
                name        = g .. ":" .. a,
                version     = ver,
                description = "Available on Maven Central"
            })
        end
    end
    return items
end

local function local_artifact_versions(groupId, artifactId)
    local path = maven_repo_dir() .. "/" .. groupId:gsub("%.", "/") .. "/" .. artifactId
    local result = reqpack.exec.run("ls -1 " .. shell_quote(path) .. " 2>/dev/null")
    local versions = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local version = trim(line)
        if version ~= "" then
            table.insert(versions, version)
        end
    end
    table.sort(versions)
    return versions
end

local function search_local_artifacts(prompt)
    local normalized = trim(prompt):lower()
    local repo = maven_repo_dir()
    local result = reqpack.exec.run("find " .. shell_quote(repo) .. " -name '*.pom' 2>/dev/null")
    local items = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local path = trim(line)
        local relative = path:gsub("^" .. repo .. "/?", "")
        local parts = split(relative, "/")
        if #parts >= 4 then
            local version = parts[#parts - 1]
            local artifactId = parts[#parts - 2]
            local groupParts = {}
            for index = 1, #parts - 3 do
                table.insert(groupParts, parts[index])
            end
            local groupId = join(groupParts, ".")
            local name = groupId .. ":" .. artifactId
            if normalized == "" or name:lower():find(normalized, 1, true) ~= nil then
                table.insert(items, {
                    name = name,
                    version = version,
                    description = "Installed in local Maven repository"
                })
            end
        end
    end
    return items
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

function plugin.getName()
    return "Apache Maven Manager"
end

function plugin.getVersion()
    return "2.0.0"
end

function plugin.getSecurityMetadata()
    return {
        osvEcosystem = "Maven",
        purlType = "maven",
        versionComparatorProfile = "maven-comparable",
    }
end

function plugin.getCategories()
    return { "Java", "Build", "Maven" }
end

plugin.fileExtensions = { ".jar", ".war", ".ear" }

function plugin.getRequirements()
    return {
        { system = "sys", name = getenv("REQPACK_MAVEN_JAVA_PACKAGE", "java") },
        { system = "sys", name = getenv("REQPACK_MAVEN_PACKAGE", "maven") }
    }
end

function plugin.getMissingPackages(packages)
    local missing = {}
    for _, pkg in ipairs(packages or {}) do
        if pkg.localTarget then
            table.insert(missing, pkg)
        else
            local artifact, err = artifact_from_pkg(pkg)
            local action = pkg.action
            if artifact == nil then
                table.insert(missing, pkg)
            elseif action == "remove" or action == 2 then
                if path_exists(artifact_repo_path(artifact)) then
                    table.insert(missing, pkg)
                end
            elseif action == "update" or action == 3 then
                if not path_exists(artifact_repo_path(artifact)) then
                    table.insert(missing, pkg)
                end
            elseif not path_exists(artifact_repo_path(artifact)) then
                table.insert(missing, pkg)
            end
        end
    end

    return missing
end

function plugin.install(context, packages)
    if #packages == 0 then return true end

    local commands = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            context.tx.failed(err)
            return false
        end

        table.insert(commands, "mvn -q dependency:get -Dtransitive=true -Dartifact=" .. shell_quote(artifact_coordinate(artifact)))
    end

    context.tx.begin_step("install maven artifacts")
    local result = context.exec.run(table.concat(commands, " && "))
    if not result.success then
        context.tx.failed("maven install failed")
        return false
    end

    context.events.installed(packages)
    context.tx.success()
    return true
end

function plugin.installLocal(context, path)
    local resolved_path, resolve_error = resolve_local_file(path)
    if resolved_path == nil then
        context.tx.failed(resolve_error)
        return false
    end

    context.tx.begin_step("install local maven artifact")
    local result = context.exec.run("mvn -q install:install-file -Dfile=" .. shell_quote(resolved_path) .. " -DgeneratePom=true")
    if not result.success then
        context.tx.failed("maven local install failed")
        return false
    end

    context.events.installed({ path = resolved_path, localTarget = true })
    context.tx.success()
    return true
end

function plugin.remove(context, packages)
    if #packages == 0 then return true end

    local paths = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            context.tx.failed(err)
            return false
        end

        table.insert(paths, shell_quote(artifact_repo_dir(artifact)))
    end

    context.tx.begin_step("remove maven artifacts")
    local result = context.exec.run("rm -rf " .. table.concat(paths, " "))
    if not result.success then
        context.tx.failed("maven remove failed")
        return false
    end

    context.events.deleted(packages)
    context.tx.success()
    return true
end

function plugin.update(context, packages)
    if packages == nil or #packages == 0 then
        context.log.info("maven update without packages does nothing")
        return true
    end

    local commands = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            context.tx.failed(err)
            return false
        end

        table.insert(commands, "mvn -q dependency:get -U -Dtransitive=true -Dartifact=" .. shell_quote(artifact_coordinate(artifact)))
    end

    context.tx.begin_step("update maven artifacts")
    local result = context.exec.run(table.concat(commands, " && "))
    if not result.success then
        context.tx.failed("maven update failed")
        return false
    end

    context.events.updated(packages)
    context.tx.success()
    return true
end

function plugin.list(context)
    local items = search_local_artifacts("")
    context.events.listed(items)
    return items
end

function plugin.search(context, prompt)
    local items = search_local_artifacts(prompt)
    if #items == 0 then
        context.log.info("no local artifacts matched, querying Maven Central...")
        items = search_maven_central(context, prompt)
    end
    if #items == 0 then
        items = {
            {
                name        = prompt,
                version     = "unknown",
                description = "No Maven artifacts matched locally or on Maven Central"
            }
        }
    end
    context.events.searched(items)
    return items
end

function plugin.info(context, name)
    local artifact, err = artifact_query_from_name(name)
    if artifact == nil then
        local item = {
            name = name,
            version = "unknown",
            description = err or "Invalid Maven artifact coordinate"
        }
        context.events.informed(item)
        return item
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
    local item = {
        name = artifact.groupId .. ":" .. artifact.artifactId,
        version = latestVersion,
        description = path_exists(repoPath) and ("Installed locally at " .. repoPath) or "Not present in local Maven repository"
    }
    context.events.informed(item)
    return item
end

function plugin.resolvePackage(context, package)
    local artifact, err = artifact_query_from_name(package.name)
    if artifact == nil then
        return nil
    end

    local resolved = package
    local repoPath = artifact_repo_path(artifact)
    if path_exists(repoPath) then
        resolved.version = artifact.version or resolved.version
        return resolved
    end

    if artifact.version == nil or trim(artifact.version) == "" then
        return nil
    end

    local search = search_maven_central(context, artifact.groupId .. ":" .. artifact.artifactId)
    for _, item in ipairs(search) do
        if item.name == artifact.groupId .. ":" .. artifact.artifactId and item.version == artifact.version then
            resolved.version = artifact.version
            return resolved
        end
    end

    return nil
end

function plugin.outdated(context)
    local installed = search_local_artifacts("")
    local items = {}
    for _, pkg in ipairs(installed) do
        local parts = split(pkg.name, ":")
        if #parts >= 2 then
            local g = parts[1]
            local a = parts[2]
            local localVersion = pkg.version
            local url = "https://search.maven.org/solrsearch/select?q=g:" ..
                        url_encode(g) .. "+AND+a:" .. url_encode(a) ..
                        "&rows=1&wt=json"
            local result = context.exec.run("curl -sf --max-time 10 " .. shell_quote(url))
            if result.success then
                local latestVersion = (result.stdout or ""):match('"latestVersion":"([^"]+)"')
                if latestVersion and latestVersion ~= localVersion then
                    table.insert(items, {
                        name        = pkg.name,
                        version     = localVersion,
                        description = "Newer version available: " .. latestVersion
                    })
                end
            end
        end
    end
    context.events.outdated(items)
    return items
end

function plugin.init()
    return reqpack.exec.run("command -v java >/dev/null 2>&1 && command -v mvn >/dev/null 2>&1").success
end

function plugin.shutdown()
    return true
end
