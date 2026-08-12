#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mv {

// Convert a filesystem path to a UTF-8 encoded std::string.
//
// std::filesystem::path::string() is deceptively dangerous on Windows: the
// path is stored internally as UTF-16, and string() converts it to the process
// active (ANSI) code page via WideCharToMultiByte(CP_ACP, ...). Any character
// that has no representation in that code page -- a curly quote, an en dash, an
// emoji, or any accented / non-Latin letter -- makes the conversion fail, and
// the STL turns that failure into a thrown std::system_error whose message is
// "No mapping for the Unicode character exists in the target multi-byte code
// page." Iterating a directory and calling string() on each entry therefore
// crashes the moment one neighbouring file has such a character, even when the
// folder being browsed is pure ASCII.
//
// UTF-8 can represent every path, so this conversion never fails. It is also
// what Dear ImGui, GLFW and the log/console expect, so routing display strings
// through here fixes garbled non-ASCII names as a bonus. On POSIX the native
// encoding is already bytes (conventionally UTF-8) and u8string() is lossless.
std::string pathToUtf8(const std::filesystem::path& path);

// Inverse of pathToUtf8: build a path from UTF-8 bytes. Use this whenever a
// path was stored as a UTF-8 std::string and needs to become a path again, so
// the round trip stays lossless for non-ASCII names.
std::filesystem::path pathFromUtf8(std::string_view utf8);

} // namespace mv
