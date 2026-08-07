#pragma once

namespace mv {

/// Installs a handler that prints a stack trace when the process dies from a
/// fatal signal, then re-raises so the exit status and any core dump are
/// unaffected.
///
/// A viewer opens files it did not create, and a malformed one can take a
/// third-party parser somewhere unrecoverable. When that happens the useful
/// information is where it happened, and a segfault on its own tells you
/// nothing. Getting that without asking anyone to reproduce under a debugger
/// is worth the small amount of machinery.
void installCrashHandler();

} // namespace mv
