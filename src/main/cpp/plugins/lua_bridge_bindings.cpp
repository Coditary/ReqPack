#include "plugins/lua_bridge_bindings.h"

#include <array>
#include "core/config/configuration.h"
#include "core/host/host_info.h"
#include "output/progress_metrics_lua.h"
#include "plugins/lua_bridge_value_mapper.h"

#include <type_traits>

namespace {

sol::table make_string_array_table(sol::state& lua, const std::vector<std::string>& values) {
    sol::table table = lua.create_table(static_cast<int>(values.size()), 0);
    for (std::size_t index = 0; index < values.size(); ++index) {
        table[static_cast<int>(index + 1)] = values[index];
    }
    return table;
}

void set_string_field_if_present(sol::table& table, const std::string& key, const std::string& value) {
    if (!value.empty()) {
        table[key] = value;
    }
}

void set_optional_string_field(sol::state& lua, sol::table& table, const std::string& key, const std::optional<std::string>& value) {
    table[key] = value.has_value() ? sol::make_object(lua, value.value()) : sol::make_object(lua, sol::lua_nil);
}

void set_optional_uint32_field(sol::state& lua, sol::table& table, const std::string& key, const std::optional<std::uint32_t>& value) {
    table[key] = value.has_value() ? sol::make_object(lua, value.value()) : sol::make_object(lua, sol::lua_nil);
}

void set_optional_uint64_field(sol::state& lua, sol::table& table, const std::string& key, const std::optional<std::uint64_t>& value) {
    table[key] = value.has_value() ? sol::make_object(lua, value.value()) : sol::make_object(lua, sol::lua_nil);
}

void set_optional_bool_field(sol::state& lua, sol::table& table, const std::string& key, const std::optional<bool>& value) {
    table[key] = value.has_value() ? sol::make_object(lua, value.value()) : sol::make_object(lua, sol::lua_nil);
}

sol::table make_host_info_table(sol::state& lua, const HostInfoSnapshot& snapshot) {
    sol::table host = lua.create_table();

    sol::table platform = lua.create_table();
    set_string_field_if_present(platform, "osFamily", snapshot.platform.osFamily);
    set_string_field_if_present(platform, "arch", snapshot.platform.arch);
    set_string_field_if_present(platform, "target", snapshot.platform.target);
    set_string_field_if_present(platform, "supportLevel", snapshot.platform.supportLevel);
    set_optional_string_field(lua, platform, "supportReason", snapshot.platform.supportReason);
    host["platform"] = platform;

    sol::table os = lua.create_table();
    set_string_field_if_present(os, "family", snapshot.os.family);
    set_string_field_if_present(os, "id", snapshot.os.id);
    set_string_field_if_present(os, "name", snapshot.os.name);
    set_optional_string_field(lua, os, "version", snapshot.os.version);
    set_optional_string_field(lua, os, "versionId", snapshot.os.versionId);
    set_optional_string_field(lua, os, "prettyName", snapshot.os.prettyName);
    set_optional_string_field(lua, os, "distroId", snapshot.os.distroId);
    set_optional_string_field(lua, os, "distroName", snapshot.os.distroName);
    host["os"] = os;

    sol::table kernel = lua.create_table();
    set_optional_string_field(lua, kernel, "name", snapshot.kernel.name);
    set_optional_string_field(lua, kernel, "release", snapshot.kernel.release);
    set_optional_string_field(lua, kernel, "version", snapshot.kernel.version);
    host["kernel"] = kernel;

    sol::table cpu = lua.create_table();
    set_string_field_if_present(cpu, "arch", snapshot.cpu.arch);
    set_optional_string_field(lua, cpu, "vendor", snapshot.cpu.vendor);
    set_optional_string_field(lua, cpu, "model", snapshot.cpu.model);
    set_optional_uint32_field(lua, cpu, "logicalCores", snapshot.cpu.logicalCores);
    set_optional_uint32_field(lua, cpu, "physicalCores", snapshot.cpu.physicalCores);
    host["cpu"] = cpu;

    sol::table memory = lua.create_table();
    set_optional_uint64_field(lua, memory, "totalBytes", snapshot.memory.totalBytes);
    set_optional_uint64_field(lua, memory, "availableBytes", snapshot.memory.availableBytes);
    host["memory"] = memory;

    sol::table gpus = lua.create_table(static_cast<int>(snapshot.gpus.size()), 0);
    for (std::size_t index = 0; index < snapshot.gpus.size(); ++index) {
        sol::table gpu = lua.create_table();
        set_optional_string_field(lua, gpu, "vendor", snapshot.gpus[index].vendor);
        set_optional_string_field(lua, gpu, "model", snapshot.gpus[index].model);
        set_optional_string_field(lua, gpu, "driverVersion", snapshot.gpus[index].driverVersion);
        set_optional_string_field(lua, gpu, "backend", snapshot.gpus[index].backend);
        gpus[static_cast<int>(index + 1)] = gpu;
    }
    host["gpus"] = gpus;

    sol::table storage = lua.create_table();
    sol::table mounts = lua.create_table(static_cast<int>(snapshot.storage.mounts.size()), 0);
    for (std::size_t index = 0; index < snapshot.storage.mounts.size(); ++index) {
        const HostMountInfo& mountInfo = snapshot.storage.mounts[index];
        sol::table mount = lua.create_table();
        set_optional_string_field(lua, mount, "device", mountInfo.device);
        set_string_field_if_present(mount, "mountPoint", mountInfo.mountPoint);
        set_optional_string_field(lua, mount, "fsType", mountInfo.fsType);
        set_optional_uint64_field(lua, mount, "totalBytes", mountInfo.totalBytes);
        set_optional_uint64_field(lua, mount, "usedBytes", mountInfo.usedBytes);
        set_optional_uint64_field(lua, mount, "availableBytes", mountInfo.availableBytes);
        set_optional_bool_field(lua, mount, "readOnly", mountInfo.readOnly);
        mounts[static_cast<int>(index + 1)] = mount;
    }
    storage["mounts"] = mounts;
    host["storage"] = storage;

    sol::table cache = lua.create_table();
    cache["schemaVersion"] = snapshot.cache.schemaVersion;
    cache["collectedAtEpoch"] = snapshot.cache.collectedAtEpoch;
    cache["expiresAtEpoch"] = snapshot.cache.expiresAtEpoch;
    set_string_field_if_present(cache, "refreshReason", snapshot.cache.refreshReason);
    set_string_field_if_present(cache, "source", snapshot.cache.source);
    host["cache"] = cache;

    return host;
}

sol::table make_repository_entry_table(sol::state& lua, const RepositoryEntry& repository) {
    sol::table entry = lua.create_table();
    entry["id"] = repository.id;
    entry["url"] = repository.url;
    entry["priority"] = repository.priority;
    entry["enabled"] = repository.enabled;
    entry["type"] = repository.type;

    sol::table auth = lua.create_table();
    auth["type"] = to_string(repository.auth.type);
    if (!repository.auth.username.empty()) {
        auth["username"] = repository.auth.username;
    }
    if (!repository.auth.password.empty()) {
        auth["password"] = repository.auth.password;
    }
    if (!repository.auth.token.empty()) {
        auth["token"] = repository.auth.token;
    }
    if (!repository.auth.sshKey.empty()) {
        auth["sshKey"] = repository.auth.sshKey;
    }
    if (!repository.auth.headerName.empty()) {
        auth["headerName"] = repository.auth.headerName;
    }
    entry["auth"] = auth;

    sol::table validation = lua.create_table();
    validation["checksum"] = to_string(repository.validation.checksum);
    validation["tlsVerify"] = repository.validation.tlsVerify;
    entry["validation"] = validation;

    sol::table scope = lua.create_table();
    scope["include"] = make_string_array_table(lua, repository.scope.include);
    scope["exclude"] = make_string_array_table(lua, repository.scope.exclude);
    entry["scope"] = scope;

    for (const auto& [key, value] : repository.extras) {
        const std::string extraKey = key;
        std::visit([&](const auto& item) {
            using ValueType = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<ValueType, std::vector<std::string>>) {
                entry[extraKey] = make_string_array_table(lua, item);
            } else {
                entry[extraKey] = item;
            }
        }, value);
    }

    return entry;
}

}  // namespace

LuaBridgeBindings::LuaBridgeBindings(LuaBridgeScriptRuntime& runtime, LuaBridgeHostRuntime& hostRuntime)
    : m_runtime(runtime), m_hostRuntime(hostRuntime) {}

void LuaBridgeBindings::registerBuiltinTypes() {
    sol::state& lua = m_runtime.state();

    lua.new_usertype<Package>(
        "Package",
        sol::constructors<Package()>(),
        "action", sol::property(
            [](const Package& package) {
                return static_cast<int>(package.action);
            },
            [](Package& package, int action) {
                package.action = static_cast<ActionType>(action);
            }
        ),
        "system", &Package::system,
        "name", &Package::name,
        "version", &Package::version,
        "sourcePath", &Package::sourcePath,
        "localTarget", &Package::localTarget,
        "flags", &Package::flags
    );

    lua.new_usertype<Request>(
        "Request",
        sol::constructors<Request()>(),
        "action", sol::property(
            [](const Request& request) {
                return static_cast<int>(request.action);
            },
            [](Request& request, int action) {
                request.action = static_cast<ActionType>(action);
            }
        ),
        "system", &Request::system,
        "packages", &Request::packages,
        "flags", &Request::flags,
        "outputFormat", &Request::outputFormat,
        "outputPath", &Request::outputPath,
        "localPath", &Request::localPath,
        "usesLocalTarget", &Request::usesLocalTarget
    );

    lua.new_usertype<PackageInfo>(
        "PackageInfo",
        sol::constructors<PackageInfo()>(),
        "system", &PackageInfo::system,
        "name", &PackageInfo::name,
        "packageId", &PackageInfo::packageId,
        "version", &PackageInfo::version,
        "latestVersion", &PackageInfo::latestVersion,
        "status", &PackageInfo::status,
        "installed", &PackageInfo::installed,
        "summary", &PackageInfo::summary,
        "description", &PackageInfo::description,
        "homepage", &PackageInfo::homepage,
        "documentation", &PackageInfo::documentation,
        "sourceUrl", &PackageInfo::sourceUrl,
        "repository", &PackageInfo::repository,
        "channel", &PackageInfo::channel,
        "section", &PackageInfo::section,
        "packageType", &PackageInfo::packageType,
        "type", &PackageInfo::packageType,
        "architecture", &PackageInfo::architecture,
        "targetSystems", &PackageInfo::targetSystems,
        "license", &PackageInfo::license,
        "author", &PackageInfo::author,
        "maintainer", &PackageInfo::maintainer,
        "email", &PackageInfo::email,
        "publishedAt", &PackageInfo::publishedAt,
        "updatedAt", &PackageInfo::updatedAt,
        "size", &PackageInfo::size,
        "installedSize", &PackageInfo::installedSize,
        "dependencies", &PackageInfo::dependencies,
        "optionalDependencies", &PackageInfo::optionalDependencies,
        "provides", &PackageInfo::provides,
        "conflicts", &PackageInfo::conflicts,
        "replaces", &PackageInfo::replaces,
        "binaries", &PackageInfo::binaries,
        "tags", &PackageInfo::tags
    );

    lua.new_usertype<ExecResult>(
        "ExecResult",
        sol::constructors<ExecResult()>(),
        "success", &ExecResult::success,
        "exitCode", &ExecResult::exitCode,
        "stdout", &ExecResult::stdoutText,
        "stderr", &ExecResult::stderrText
    );
}

void LuaBridgeBindings::registerContextTypes() {
    sol::state& lua = m_runtime.state();

    lua.new_usertype<PluginCallContext>(
        "PluginCallContext",
        "flags", sol::readonly_property([](const PluginCallContext& context) {
            return context.flags;
        }),
        "plugin", sol::readonly_property([&lua](const PluginCallContext& context) {
            sol::table plugin = lua.create_table();
            plugin["id"] = context.pluginId;
            plugin["dir"] = context.pluginDirectory;
            plugin["script"] = context.scriptPath;
            return plugin;
        }),
        "repositories", sol::readonly_property([&lua](const PluginCallContext& context) {
            sol::table repositories = lua.create_table(static_cast<int>(context.repositories.size()), 0);
            for (std::size_t index = 0; index < context.repositories.size(); ++index) {
                repositories[static_cast<int>(index + 1)] = make_repository_entry_table(lua, context.repositories[index]);
            }
            return repositories;
        }),
        "host", sol::readonly_property([&lua](const PluginCallContext& context) {
            const std::shared_ptr<const HostInfoSnapshot> snapshot = context.hostInfo != nullptr ? context.hostInfo : HostInfoService::currentSnapshot();
            return make_host_info_table(lua, *snapshot);
        }),
        "proxy", sol::readonly_property([&lua](const PluginCallContext& context) {
            if (!context.proxy.has_value()) {
                return sol::make_object(lua, sol::lua_nil);
            }

            sol::table proxy = lua.create_table();
            if (!context.proxy->defaultTarget.empty()) {
                proxy["default"] = context.proxy->defaultTarget;
            }
            proxy["targets"] = make_string_array_table(lua, context.proxy->targets);

            sol::table options = lua.create_table();
            for (const auto& [key, value] : context.proxy->options) {
                options[key] = value;
            }
            proxy["options"] = options;

            return sol::make_object(lua, proxy);
        }),
        "log", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table log = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            log.set_function("debug", [this, contextId](const std::string& message) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->logDebug(binding->pluginId, message);
                }
            });
            log.set_function("info", [this, contextId](const std::string& message) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->logInfo(binding->pluginId, message);
                }
            });
            log.set_function("warn", [this, contextId](const std::string& message) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->logWarn(binding->pluginId, message);
                }
            });
            log.set_function("error", [this, contextId](const std::string& message) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->logError(binding->pluginId, message);
                }
            });
            return log;
        }),
        "tx", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table tx = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            tx.set_function("status", [this, contextId](int code) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->emitStatus(binding->sourceId, code);
                }
            });
            tx.set_function("progress", [this, contextId](sol::object payload) {
                if (const std::optional<DisplayProgressMetrics> metrics = progress_metrics_from_lua_object(payload); metrics.has_value()) {
                    if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                        binding->host->emitProgress(binding->sourceId, metrics.value());
                    }
                }
            });
            tx.set_function("begin_step", [this, contextId](const std::string& label) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->emitBeginStep(binding->sourceId, label);
                }
            });
            tx.set_function("commit", [this, contextId]() {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->emitCommit(binding->sourceId);
                }
            });
            tx.set_function("success", [this, contextId]() {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->emitSuccess(binding->sourceId);
                }
            });
            tx.set_function("failed", [this, contextId](const std::string& message) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->emitFailure(binding->sourceId, message);
                }
            });
            return tx;
        }),
        "events", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table events = lua.create_table();
            const std::array<const char*, 8> names{"installed", "deleted", "updated", "listed", "searched", "informed", "outdated", "unavailable"};
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            for (const char* name : names) {
                events.set_function(name, [this, contextId, name](sol::object payload) {
                    if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                        binding->host->emitEvent(binding->sourceId, name, LuaBridgeValueMapper::serializeLuaPayload(payload));
                    }
                });
            }
            return events;
        }),
        "artifacts", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table artifacts = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            artifacts.set_function("register", [this, contextId](sol::object payload) {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    binding->host->registerArtifact(binding->pluginId, LuaBridgeValueMapper::serializeLuaPayload(payload));
                }
            });
            return artifacts;
        }),
        "exec", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table exec = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            exec.set_function("run", sol::overload(
                [this, contextId](const std::string& command) {
                    if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                        return binding->host->execute(binding->sourceId, command);
                    }
                    return ExecResult{};
                },
                [this, contextId](const std::string& command, const sol::object& rules) {
                    const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId);
                    if (binding == nullptr) {
                        return ExecResult{};
                    }
                    return m_hostRuntime.executeCommandWithPolicy(
                        binding->sourceId,
                        command,
                        rules,
                        m_hostRuntime.hasSilentRuntimeFlag(binding->flags)
                    );
                }
            ));
            return exec;
        }),
        "fs", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table fs = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            fs.set_function("get_tmp_dir", [this, contextId]() {
                if (const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId); binding != nullptr && binding->host != nullptr) {
                    return binding->host->createTempDirectory(binding->pluginId);
                }
                return std::string{};
            });
            return fs;
        }),
        "net", sol::readonly_property([this, &lua](const PluginCallContext& context) {
            sol::table net = lua.create_table();
            const std::uint64_t contextId = m_hostRuntime.retainRuntimeBindingContext(context);
            net.set_function("download", [this, contextId](const std::string& url, const std::string& destinationPath) {
                const LuaBridgeRuntimeBindingContext* binding = m_hostRuntime.runtimeBindingContext(contextId);
                const DownloadResult result = binding != nullptr && binding->host != nullptr
                    ? binding->host->download(binding->pluginId, url, destinationPath)
                    : DownloadResult{};
                return result.success;
            });
            return net;
        })
    );
}

void LuaBridgeBindings::registerReqpackNamespace() {
    sol::state& lua = m_runtime.state();
    sol::table reqpack = lua.create_named_table("reqpack");
    sol::table exec = lua.create_table();
    exec.set_function("run", [this](const std::string& command) {
        return m_hostRuntime.runCommand(command);
    });
    reqpack["exec"] = exec;
    reqpack["host"] = make_host_info_table(lua, *HostInfoService::currentSnapshot());
}
