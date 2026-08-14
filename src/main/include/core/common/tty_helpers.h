#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

inline bool reqpack_stdout_is_tty() {
#if defined(_WIN32)
    DWORD consoleMode = 0;
    const HANDLE stdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    return stdOut != INVALID_HANDLE_VALUE && GetConsoleMode(stdOut, &consoleMode);
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}
