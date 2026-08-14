#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

inline std::size_t reqpack_terminal_column_count(std::size_t fallback = 100) {
    if (const char* columns = std::getenv("COLUMNS")) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(columns, &end, 10);
        if (end != columns && end != nullptr && *end == '\0' && parsed > 0) {
            return static_cast<std::size_t>(parsed);
        }
    }

#if defined(_WIN32)
    DWORD consoleMode = 0;
    const HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdOut != INVALID_HANDLE_VALUE && GetConsoleMode(stdOut, &consoleMode)) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(stdOut, &info)) {
            const int width = info.srWindow.Right - info.srWindow.Left + 1;
            if (width > 0) {
                return static_cast<std::size_t>(width);
            }
        }
    }
#else
    winsize size{};
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return static_cast<std::size_t>(size.ws_col);
    }
#endif

    return fallback;
}
