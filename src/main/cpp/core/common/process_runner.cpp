#include "core/common/process_runner.h"

#include "core/common/network_environment.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/common/pipe_helpers.h"
#endif

namespace {

#if defined(_WIN32)

std::string quote_windows_argument(const std::string& argument) {
    if (argument.empty()) {
        return "\"\"";
    }

    bool needsQuotes = false;
    for (char ch : argument) {
        if (ch == ' ' || ch == '\t' || ch == '"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return argument;
    }

    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
            quoted.push_back('"');
            continue;
        }
        if (backslashes != 0) {
            quoted.append(backslashes, '\\');
            backslashes = 0;
        }
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

std::string build_windows_command_line(const std::vector<std::string>& arguments) {
    std::string commandLine;
    for (const std::string& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine += quote_windows_argument(argument);
    }
    return commandLine;
}

std::vector<char> build_windows_environment_block(const std::vector<std::string>& environment) {
    std::size_t totalSize = 1;
    for (const std::string& entry : environment) {
        totalSize += entry.size() + 1;
    }

    std::vector<char> block(totalSize, '\0');
    std::size_t offset = 0;
    for (const std::string& entry : environment) {
        std::memcpy(block.data() + offset, entry.data(), entry.size());
        offset += entry.size() + 1;
    }
    return block;
}

HANDLE open_windows_nul_handle() {
    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    return CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ,
        &securityAttributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
}

bool read_windows_pipe(HANDLE pipe, std::string& output, std::string& errorText) {
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                return true;
            }
            errorText = "read failed: Windows error " + std::to_string(error);
            return false;
        }
        if (bytesRead == 0) {
            return true;
        }
        output.append(buffer.data(), bytesRead);
    }
}

struct WindowsProcessHandles {
    HANDLE process{nullptr};
    HANDLE thread{nullptr};
    HANDLE stdoutRead{nullptr};
    HANDLE stderrRead{nullptr};
    HANDLE stdinHandle{nullptr};
    HANDLE stdoutWrite{nullptr};
    HANDLE stderrWrite{nullptr};
};

void close_windows_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

void close_windows_process_handles(WindowsProcessHandles& handles) {
    close_windows_handle(handles.stdoutWrite);
    close_windows_handle(handles.stderrWrite);
    close_windows_handle(handles.stdinHandle);
    close_windows_handle(handles.stdoutRead);
    close_windows_handle(handles.stderrRead);
    close_windows_handle(handles.thread);
    close_windows_handle(handles.process);
}

std::optional<WindowsProcessHandles> start_windows_process(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory,
    bool captureOutput,
    std::string& errorText
) {
    if (arguments.empty()) {
        errorText = "empty command";
        return std::nullopt;
    }

    SECURITY_ATTRIBUTES securityAttributes{};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.bInheritHandle = TRUE;

    WindowsProcessHandles handles{};

    if (captureOutput) {
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrRead = nullptr;
        HANDLE stderrWrite = nullptr;
        if (!CreatePipe(&stdoutRead, &stdoutWrite, &securityAttributes, 0) ||
            !CreatePipe(&stderrRead, &stderrWrite, &securityAttributes, 0)) {
            errorText = "failed to create pipe: Windows error " + std::to_string(GetLastError());
            close_windows_handle(stdoutRead);
            close_windows_handle(stdoutWrite);
            close_windows_handle(stderrRead);
            close_windows_handle(stderrWrite);
            return std::nullopt;
        }
        if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
            errorText = "failed to configure pipe inheritance: Windows error " + std::to_string(GetLastError());
            close_windows_handle(stdoutRead);
            close_windows_handle(stdoutWrite);
            close_windows_handle(stderrRead);
            close_windows_handle(stderrWrite);
            return std::nullopt;
        }
        handles.stdoutRead = stdoutRead;
        handles.stderrRead = stderrRead;
        handles.stdoutWrite = stdoutWrite;
        handles.stderrWrite = stderrWrite;
    }

    handles.stdinHandle = open_windows_nul_handle();
    if (handles.stdinHandle == INVALID_HANDLE_VALUE) {
        errorText = "failed to open NUL: Windows error " + std::to_string(GetLastError());
        close_windows_process_handles(handles);
        return std::nullopt;
    }

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdInput = handles.stdinHandle;
    startupInfo.hStdOutput = captureOutput ? handles.stdoutWrite : GetStdHandle(STD_OUTPUT_HANDLE);
    startupInfo.hStdError = captureOutput ? handles.stderrWrite : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION processInfo{};
    std::string commandLine = build_windows_command_line(arguments);
    std::vector<char> commandLineBuffer(commandLine.begin(), commandLine.end());
    commandLineBuffer.push_back('\0');

    std::vector<std::string> environmentStorage = reqpack_sanitized_process_environment();
    std::vector<char> environmentBlock = build_windows_environment_block(environmentStorage);

    const std::string workingDirectoryString = workingDirectory.empty() ? std::string{} : workingDirectory.string();
    const char* workingDirectoryPointer = workingDirectoryString.empty() ? nullptr : workingDirectoryString.c_str();

    const BOOL created = CreateProcessA(
        arguments.front().c_str(),
        commandLineBuffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        environmentBlock.data(),
        workingDirectoryPointer,
        &startupInfo,
        &processInfo
    );

    close_windows_handle(handles.stdoutWrite);
    close_windows_handle(handles.stderrWrite);
    close_windows_handle(handles.stdinHandle);

    if (!created) {
        errorText = "spawn failed: Windows error " + std::to_string(GetLastError());
        close_windows_handle(handles.stdoutRead);
        close_windows_handle(handles.stderrRead);
        return std::nullopt;
    }

    handles.process = processInfo.hProcess;
    handles.thread = processInfo.hThread;
    return handles;
}

bool wait_for_windows_process(HANDLE process, int& exitCode, std::string& errorText) {
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        errorText = "wait failed: Windows error " + std::to_string(GetLastError());
        return false;
    }

    DWORD windowsExitCode = 1;
    if (!GetExitCodeProcess(process, &windowsExitCode)) {
        errorText = "failed to read exit code: Windows error " + std::to_string(GetLastError());
        return false;
    }

    exitCode = static_cast<int>(windowsExitCode);
    return true;
}

#else

bool wait_for_posix_process(pid_t pid, int& exitCode, std::string& errorText) {
    int status = 0;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            errorText = std::string{"waitpid failed: "} + std::strerror(errno);
            return false;
        }
    }

    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
        return true;
    }

    if (WIFSIGNALED(status)) {
        exitCode = 128 + WTERMSIG(status);
        errorText = "process terminated by signal";
        return false;
    }

    exitCode = 1;
    errorText = "process exited abnormally";
    return false;
}

bool read_posix_fd(int fd, std::string& output, std::string& errorText) {
    char buffer[4096];
    while (true) {
        const ssize_t bytesRead = ::read(fd, buffer, sizeof(buffer));
        if (bytesRead > 0) {
            output.append(buffer, static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        errorText += std::string{"read failed: "} + std::strerror(errno);
        return false;
    }
}

std::optional<pid_t> start_posix_process(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory,
    int stdoutReadFd,
    int stdoutWriteFd,
    int stderrReadFd,
    int stderrWriteFd,
    std::string& errorText
) {
    posix_spawn_file_actions_t fileActions;
    if (posix_spawn_file_actions_init(&fileActions) != 0) {
        errorText = "posix_spawn_file_actions_init failed";
        return std::nullopt;
    }

    bool ready = true;
#if defined(__linux__) || defined(__APPLE__)
    if (!workingDirectory.empty()) {
        ready = posix_spawn_file_actions_addchdir_np(&fileActions, workingDirectory.c_str()) == 0;
    }
#endif

    if (ready && stdoutWriteFd >= 0 && stderrWriteFd >= 0) {
        ready =
            posix_spawn_file_actions_addopen(&fileActions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
            posix_spawn_file_actions_adddup2(&fileActions, stdoutWriteFd, STDOUT_FILENO) == 0 &&
            posix_spawn_file_actions_adddup2(&fileActions, stderrWriteFd, STDERR_FILENO) == 0 &&
            posix_spawn_file_actions_addclose(&fileActions, stdoutReadFd) == 0 &&
            posix_spawn_file_actions_addclose(&fileActions, stdoutWriteFd) == 0 &&
            posix_spawn_file_actions_addclose(&fileActions, stderrReadFd) == 0 &&
            posix_spawn_file_actions_addclose(&fileActions, stderrWriteFd) == 0;
    }

    if (!ready) {
        posix_spawn_file_actions_destroy(&fileActions);
        errorText = "failed to configure process spawn";
        return std::nullopt;
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    std::vector<std::string> environmentStorage = reqpack_sanitized_process_environment();
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environmentStorage.size() + 1);
    for (std::string& entry : environmentStorage) {
        environmentPointers.push_back(entry.data());
    }
    environmentPointers.push_back(nullptr);

    pid_t pid = 0;
    const int spawnResult = posix_spawnp(
        &pid,
        arguments.front().c_str(),
        &fileActions,
        nullptr,
        argv.data(),
        environmentPointers.data()
    );
    posix_spawn_file_actions_destroy(&fileActions);
    if (spawnResult != 0) {
        errorText = std::string{"spawn failed: "} + std::strerror(spawnResult);
        return std::nullopt;
    }

    return pid;
}

#endif

}  // namespace

bool reqpack_run_process(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory
) {
#if defined(_WIN32)
    std::string errorText;
    std::optional<WindowsProcessHandles> handles = start_windows_process(arguments, workingDirectory, false, errorText);
    if (!handles.has_value()) {
        return false;
    }

    int exitCode = 1;
    const bool waited = wait_for_windows_process(handles->process, exitCode, errorText);
    close_windows_process_handles(handles.value());
    return waited && exitCode == 0;
#else
    std::string errorText;
    const std::optional<pid_t> pid = start_posix_process(arguments, workingDirectory, -1, -1, -1, -1, errorText);
    if (!pid.has_value()) {
        return false;
    }

    int exitCode = 1;
    const bool waited = wait_for_posix_process(pid.value(), exitCode, errorText);
    return waited && exitCode == 0;
#endif
}

ReqpackProcessResult reqpack_run_process_capture(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& workingDirectory
) {
    ReqpackProcessResult result;
    if (arguments.empty()) {
        result.stderrText = "empty command";
        return result;
    }

#if defined(_WIN32)
    std::string errorText;
    std::optional<WindowsProcessHandles> handles = start_windows_process(arguments, workingDirectory, true, errorText);
    if (!handles.has_value()) {
        result.stderrText = errorText;
        return result;
    }

    (void)read_windows_pipe(handles->stdoutRead, result.stdoutText, result.stderrText);
    (void)read_windows_pipe(handles->stderrRead, result.stderrText, result.stderrText);

    int exitCode = 1;
    if (!wait_for_windows_process(handles->process, exitCode, errorText)) {
        if (result.stderrText.empty()) {
            result.stderrText = errorText;
        } else {
            result.stderrText += "\n" + errorText;
        }
    }
    result.exitCode = exitCode;
    close_windows_process_handles(handles.value());
    return result;
#else
    int stdoutPipe[2];
    if (!create_pipe_cloexec(stdoutPipe)) {
        result.stderrText = std::string{"pipe(stdout) failed: "} + std::strerror(errno);
        return result;
    }

    int stderrPipe[2];
    if (!create_pipe_cloexec(stderrPipe)) {
        result.stderrText = std::string{"pipe(stderr) failed: "} + std::strerror(errno);
        (void)::close(stdoutPipe[0]);
        (void)::close(stdoutPipe[1]);
        return result;
    }

    std::string errorText;
    const std::optional<pid_t> pid = start_posix_process(
        arguments,
        workingDirectory,
        stdoutPipe[0],
        stdoutPipe[1],
        stderrPipe[0],
        stderrPipe[1],
        errorText
    );
    (void)::close(stdoutPipe[1]);
    (void)::close(stderrPipe[1]);
    if (!pid.has_value()) {
        result.stderrText = errorText;
        (void)::close(stdoutPipe[0]);
        (void)::close(stderrPipe[0]);
        return result;
    }

    (void)read_posix_fd(stdoutPipe[0], result.stdoutText, result.stderrText);
    (void)::close(stdoutPipe[0]);
    (void)read_posix_fd(stderrPipe[0], result.stderrText, result.stderrText);
    (void)::close(stderrPipe[0]);

    int exitCode = 1;
    if (!wait_for_posix_process(pid.value(), exitCode, errorText)) {
        if (result.stderrText.empty()) {
            result.stderrText = errorText;
        } else {
            result.stderrText += "\n" + errorText;
        }
    }
    result.exitCode = exitCode;
    return result;
#endif
}
