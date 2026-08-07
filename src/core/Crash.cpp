#include "core/Crash.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#    include <execinfo.h>
#    include <unistd.h>
#elif defined(_WIN32)
#    include <windows.h>
#endif

namespace mv {
namespace {

#if defined(__linux__)

const char* signalName(int signum)
{
    switch (signum)
    {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGBUS:  return "SIGBUS (bus error)";
        default:      return "unknown signal";
    }
}

/// Signal handlers may only call async-signal-safe functions, which rules out
/// printf and anything that allocates. write() is safe, and
/// backtrace_symbols_fd is specifically designed for this -- unlike
/// backtrace_symbols, which mallocs and can therefore deadlock if the crash
/// happened inside the allocator.
void writeAll(int fd, const char* text)
{
    const size_t length = std::strlen(text);
    ssize_t      written = 0;
    while (written < static_cast<ssize_t>(length))
    {
        const ssize_t n = ::write(fd, text + written, length - static_cast<size_t>(written));
        if (n <= 0) break;
        written += n;
    }
}

void handler(int signum)
{
    constexpr int kMaxFrames = 64;
    void*         frames[kMaxFrames];

    writeAll(STDERR_FILENO, "\n=====================================================\n");
    writeAll(STDERR_FILENO, "ModelViewer crashed: ");
    writeAll(STDERR_FILENO, signalName(signum));
    writeAll(STDERR_FILENO, "\n=====================================================\n");

    const int count = ::backtrace(frames, kMaxFrames);
    ::backtrace_symbols_fd(frames, count, STDERR_FILENO);

    writeAll(STDERR_FILENO,
             "\nPlease report the frames above, along with the file being opened.\n"
             "Build with -DCMAKE_BUILD_TYPE=Debug for function names and line numbers.\n\n");

    // Restore the default and re-raise, so the exit status is what it would
    // have been and any configured core dump is still produced.
    std::signal(signum, SIG_DFL);
    ::raise(signum);
}

#elif defined(_WIN32)

LONG WINAPI exceptionFilter(EXCEPTION_POINTERS* info)
{
    std::fprintf(stderr,
                 "\n=====================================================\n"
                 "ModelViewer crashed: exception 0x%08lx at 0x%p\n"
                 "=====================================================\n",
                 info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress);

    void*        frames[64];
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
        std::fprintf(stderr, "  [%02u] %p\n", i, frames[i]);

    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

#endif

} // namespace

void installCrashHandler()
{
#if defined(__linux__)
    std::signal(SIGSEGV, handler);
    std::signal(SIGABRT, handler);
    std::signal(SIGFPE, handler);
    std::signal(SIGILL, handler);
    std::signal(SIGBUS, handler);
#elif defined(_WIN32)
    SetUnhandledExceptionFilter(exceptionFilter);
#endif
}

} // namespace mv
