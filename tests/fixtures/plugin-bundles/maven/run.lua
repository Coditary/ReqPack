plugin = {}

local function trim(value)
    return (tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function escape_lua_pattern(value)
    return (tostring(value or ""):gsub("([%^%$%(%)%%%.%[%]%*%+%-%?])", "%%%1"))
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
        version = version ~= "" and version or nil,
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

local function read_text_file(path)
    local result = reqpack.exec.run("test -f " .. shell_quote(path) .. " && cat " .. shell_quote(path))
    if not result.success then
        return nil
    end
    return result.stdout or ""
end

local function pom_tag_value(pomText, tagName)
    if pomText == nil or trim(tagName) == "" then
        return ""
    end
    local value = pomText:match("<" .. tagName .. ">([\0-\255]-)</" .. tagName .. ">")
    if value == nil then
        return ""
    end
    value = value:gsub("<!%[CDATA%[([\0-\255]-)%]%]>", "%1")
    value = value:gsub("<[^>]+>", " ")
    value = value:gsub("%s+", " ")
    return trim(value)
end

local function short_summary(name, description)
    local summary = trim(name)
    if summary ~= "" and summary:find("${", 1, true) == nil then
        return summary
    end
    local normalized = trim(description)
    if normalized == "" then
        return ""
    end
    local sentence = normalized:match("^(.-[%.%!%?])%s+") or normalized
    sentence = trim(sentence)
    if #sentence > 120 then
        return sentence:sub(1, 117) .. "..."
    end
    return sentence
end

local function local_artifact_metadata(groupId, artifactId, version)
    local pomArtifact = {
        groupId = groupId,
        artifactId = artifactId,
        packaging = "pom",
        classifier = nil,
        version = version,
    }
    local pomText = read_text_file(artifact_repo_path(pomArtifact))
    if pomText == nil then
        return {
            packageType = "",
            summary = "",
            description = "",
        }
    end
    local packageType = pom_tag_value(pomText, "packaging")
    if packageType == "" then
        packageType = "jar"
    end
    local description = pom_tag_value(pomText, "description")
    return {
        packageType = packageType,
        summary = short_summary(pom_tag_value(pomText, "name"), description),
        description = description,
    }
end

local LUA_PATTERN_MAGIC = {
    ["^"] = true,
    ["$"] = true,
    ["("] = true,
    [")"] = true,
    ["%"] = true,
    ["."] = true,
    ["["] = true,
    ["]"] = true,
    ["+"] = true,
    ["-"] = true,
}

local function glob_to_lua_pattern(glob)
    local parts = { "^" }
    local value = tostring(glob or "")
    for index = 1, #value do
        local char = value:sub(index, index)
        if char == "*" then
            table.insert(parts, ".*")
        elseif char == "?" then
            table.insert(parts, ".")
        elseif LUA_PATTERN_MAGIC[char] then
            table.insert(parts, "%" .. char)
        else
            table.insert(parts, char)
        end
    end
    table.insert(parts, "$")
    return table.concat(parts)
end

local function glob_matches(glob, value)
    local normalized = trim(glob)
    if normalized == "" then
        return false
    end
    return tostring(value or ""):match(glob_to_lua_pattern(normalized)) ~= nil
end

local function artifact_scope_name(artifact)
    return artifact.groupId .. ":" .. artifact.artifactId
end

local function artifact_is_snapshot(artifact)
    local version = trim(artifact.version or "")
    return version ~= "" and version:match("%-SNAPSHOT$") ~= nil
end

local function repository_scope_matches(repo, artifact)
    local scope = repo.scope or {}
    local include = scope.include or {}
    local exclude = scope.exclude or {}
    local artifactName = artifact_scope_name(artifact)
    local included = #include == 0
    for _, pattern in ipairs(include) do
        if glob_matches(pattern, artifactName) then
            included = true
            break
        end
    end
    if not included then
        return false
    end
    for _, pattern in ipairs(exclude) do
        if glob_matches(pattern, artifactName) then
            return false
        end
    end
    return true
end

local function repository_matches_artifact(repo, artifact)
    if repo == nil or repo.enabled == false then
        return false
    end
    if not repository_scope_matches(repo, artifact) then
        return false
    end
    if artifact_is_snapshot(artifact) then
        return repo.snapshots ~= false
    end
    return repo.releases ~= false
end

local function active_repositories(context, artifact)
    local configured = context.repositories or {}
    if #configured == 0 then
        return {}, false
    end
    local matched = {}
    for _, repo in ipairs(configured) do
        if repository_matches_artifact(repo, artifact) then
            table.insert(matched, repo)
        end
    end
    return matched, true
end

local function repository_checksum_policy(repo)
    local validation = repo.validation or {}
    local checksum = trim(validation.checksum or "warn")
    if checksum == "fail" or checksum == "warn" then
        return checksum
    end
    return ""
end

local function repository_layout(repo)
    local layout = trim(repo.layout or "")
    if layout == "" then
        return "default"
    end
    return layout
end

local function xml_escape(value)
    local escaped = tostring(value or "")
    escaped = escaped:gsub("&", "&amp;")
    escaped = escaped:gsub("<", "&lt;")
    escaped = escaped:gsub(">", "&gt;")
    escaped = escaped:gsub('"', "&quot;")
    escaped = escaped:gsub("'", "&apos;")
    return escaped
end

local function repository_policy_xml(enabled, checksumPolicy)
    local xml = "<enabled>" .. (enabled and "true" or "false") .. "</enabled>"
    if checksumPolicy ~= "" then
        xml = xml .. "<checksumPolicy>" .. xml_escape(checksumPolicy) .. "</checksumPolicy>"
    end
    return xml
end

local function repository_server_xml(repo)
    local auth = repo.auth or {}
    local authType = trim(auth.type or "none")
    if authType == "basic" then
        return "<server><id>" .. xml_escape(repo.id) .. "</id>" ..
            "<username>" .. xml_escape(auth.username or "") .. "</username>" ..
            "<password>" .. xml_escape(auth.password or "") .. "</password></server>"
    end
    if authType == "token" then
        local headerName = trim(auth.headerName or "")
        local headerValue = trim(auth.token or "")
        if headerName == "" then
            headerName = "Authorization"
            headerValue = "Bearer " .. headerValue
        end
        return "<server><id>" .. xml_escape(repo.id) .. "</id><configuration><httpHeaders><property>" ..
            "<name>" .. xml_escape(headerName) .. "</name>" ..
            "<value>" .. xml_escape(headerValue) .. "</value>" ..
            "</property></httpHeaders></configuration></server>"
    end
    return ""
end

local function repository_xml(repo)
    local checksumPolicy = repository_checksum_policy(repo)
    local releasesEnabled = repo.releases ~= false
    local snapshotsEnabled = repo.snapshots ~= false
    return "<repository>" ..
        "<id>" .. xml_escape(repo.id) .. "</id>" ..
        "<url>" .. xml_escape(repo.url) .. "</url>" ..
        "<layout>" .. xml_escape(repository_layout(repo)) .. "</layout>" ..
        "<releases>" .. repository_policy_xml(releasesEnabled, checksumPolicy) .. "</releases>" ..
        "<snapshots>" .. repository_policy_xml(snapshotsEnabled, checksumPolicy) .. "</snapshots>" ..
        "</repository>"
end

local function write_maven_settings(context, repositories)
    local tmp = context.fs.get_tmp_dir()
    if trim(tmp) == "" then
        return nil, "failed to create maven settings directory"
    end
    local repositoriesXml = {}
    local serversXml = {}
    for _, repo in ipairs(repositories) do
        table.insert(repositoriesXml, repository_xml(repo))
        local serverXml = repository_server_xml(repo)
        if serverXml ~= "" then
            table.insert(serversXml, serverXml)
        end
    end
    local settingsXml = "<?xml version='1.0' encoding='UTF-8'?>" ..
        "<settings xmlns='http://maven.apache.org/SETTINGS/1.0.0' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance'" ..
        " xsi:schemaLocation='http://maven.apache.org/SETTINGS/1.0.0 https://maven.apache.org/xsd/settings-1.0.0.xsd'>" ..
        "<servers>" .. join(serversXml, "") .. "</servers>" ..
        "<profiles><profile><id>reqpack</id><repositories>" .. join(repositoriesXml, "") ..
        "</repositories></profile></profiles><activeProfiles><activeProfile>reqpack</activeProfile></activeProfiles></settings>"
    local settingsPath = tmp .. "/settings.xml"
    local writeResult = context.exec.run("printf '%s' " .. shell_quote(settingsXml) .. " > " .. shell_quote(settingsPath))
    if not writeResult.success then
        return nil, "failed to write maven settings"
    end
    return settingsPath, nil
end

local function repositories_need_insecure_tls(repositories)
    for _, repo in ipairs(repositories) do
        if repo.validation ~= nil and repo.validation.tlsVerify == false then
            return true
        end
    end
    return false
end

local function remote_repository_specs(repositories)
    local specs = {}
    for _, repo in ipairs(repositories) do
        table.insert(specs, repo.id .. "::" .. repository_layout(repo) .. "::" .. repo.url)
    end
    return specs
end

local function dependency_get_command(context, artifact, forceUpdate)
    local repositories, hasConfiguredRepositories = active_repositories(context, artifact)
    if hasConfiguredRepositories and #repositories == 0 then
        return nil, "no configured maven repository matched " .. artifact_scope_name(artifact)
    end
    local command = "mvn -q -Dmaven.repo.local=" .. shell_quote(maven_repo_dir())
    if forceUpdate then
        command = command .. " -U"
    end
    if #repositories > 0 then
        local settingsPath, settingsError = write_maven_settings(context, repositories)
        if settingsPath == nil then
            return nil, settingsError
        end
        command = command .. " -s " .. shell_quote(settingsPath)
        command = command .. " -DremoteRepositories=" .. shell_quote(join(remote_repository_specs(repositories), ","))
        if repositories_need_insecure_tls(repositories) then
            command = command .. " -Dmaven.wagon.http.ssl.insecure=true -Dmaven.wagon.http.ssl.allowall=true"
        end
    end
    command = command .. " dependency:get -Dtransitive=true -Dartifact=" .. shell_quote(artifact_coordinate(artifact))
    return command, nil
end

local function maven_central_latest_version(context, groupId, artifactId)
    local url = "https://search.maven.org/solrsearch/select?q=g:" .. groupId .. "+AND+a:" .. artifactId .. "&rows=1&wt=json"
    local result = context.exec.run("curl -sf --max-time 10 " .. shell_quote(url))
    if not result.success then
        return ""
    end
    return trim((result.stdout or ""):match('"latestVersion":"([^"]+)"') or "")
end

local function local_artifact_versions(groupId, artifactId)
    local path = maven_repo_dir() .. "/" .. groupId:gsub("%.", "/") .. "/" .. artifactId
    local result = reqpack.exec.run("ls -1 " .. shell_quote(path) .. " 2>/dev/null")
    local versions = {}
    for line in (result.stdout or ""):gmatch("[^\r\n]+") do
        local version = trim(line)
        if version ~= "" and path_exists(artifact_repo_path({ groupId = groupId, artifactId = artifactId, packaging = "pom", classifier = nil, version = version })) then
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
        local relative = path:gsub("^" .. escape_lua_pattern(repo) .. "/?", "")
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
                local metadata = local_artifact_metadata(groupId, artifactId, version)
                table.insert(items, {
                    name = name,
                    version = version,
                    type = metadata.packageType,
                    summary = metadata.summary,
                    description = metadata.description,
                })
            end
        end
    end
    return items
end

function plugin.getName()
    return "Frozen Maven Fixture"
end

function plugin.getVersion()
    return "1.0.0"
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
        { system = "sys", name = getenv("REQPACK_MAVEN_PACKAGE", "maven") },
    }
end

function plugin.getMissingPackages(packages)
    local missing = {}
    for _, pkg in ipairs(packages or {}) do
        if pkg.localTarget then
            table.insert(missing, pkg)
        else
            local artifact = artifact_from_pkg(pkg)
            if artifact == nil or not path_exists(artifact_repo_path(artifact)) then
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
        local command, command_error = dependency_get_command(context, artifact, false)
        if command == nil then
            context.tx.failed(command_error)
            return false
        end
        table.insert(commands, command)
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
    return true
end

function plugin.remove(context, packages)
    return true
end

function plugin.update(context, packages)
    if packages == nil or #packages == 0 then
        return true
    end
    local commands = {}
    for _, pkg in ipairs(packages) do
        local artifact, err = artifact_from_pkg(pkg)
        if artifact == nil then
            context.tx.failed(err)
            return false
        end
        local command, command_error = dependency_get_command(context, artifact, true)
        if command == nil then
            context.tx.failed(command_error)
            return false
        end
        table.insert(commands, command)
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
        items = {
            {
                name = prompt,
                version = "unknown",
                description = "No Maven artifacts matched locally or on Maven Central",
            }
        }
    end
    context.events.searched(items)
    return items
end

function plugin.info(context, name)
    local item = {
        name = name,
        version = "unknown",
        description = "Not present in local Maven repository",
    }
    context.events.informed(item)
    return item
end

function plugin.resolvePackage(context, package)
    return nil
end

function plugin.outdated(context)
    local installed = search_local_artifacts("")
    local items = {}
    local seen = {}
    for _, pkg in ipairs(installed) do
        local parts = split(pkg.name, ":")
        if #parts >= 2 and not seen[pkg.name] then
            seen[pkg.name] = true
            local g = parts[1]
            local a = parts[2]
            local versions = local_artifact_versions(g, a)
            local localVersion = #versions > 0 and versions[#versions] or pkg.version
            local metadata = local_artifact_metadata(g, a, localVersion)
            local latestVersion = maven_central_latest_version(context, g, a)
            local hasLatestLocally = false
            for _, version in ipairs(versions) do
                if version == latestVersion then
                    hasLatestLocally = true
                    break
                end
            end
            if latestVersion ~= "" and latestVersion ~= localVersion and not hasLatestLocally then
                table.insert(items, {
                    name = pkg.name,
                    version = localVersion,
                    latestVersion = latestVersion,
                    type = metadata.packageType,
                    summary = metadata.summary,
                    description = metadata.description,
                })
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
