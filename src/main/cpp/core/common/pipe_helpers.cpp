#include "core/common/pipe_helpers.h"

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
#include <unistd.h>
#endif

bool create_pipe_cloexec(int pipefd[2]) {
#if defined(_WIN32)
    (void)pipefd;
    return false;
#elif defined(__linux__) || defined(__APPLE__)
    if (::pipe2(pipefd, O_CLOEXEC) == 0) {
        return true;
    }
    if (::pipe(pipefd) != 0) {
        return false;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    return true;
#else
    if (::pipe(pipefd) != 0) {
        return false;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}
