#include "core/Utf8.hpp"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace mv {

#if defined(_WIN32)

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::wstring& wide = path.native(); // UTF-16 on Windows.
    if (wide.empty()) return {};

    // CP_UTF8 requires dwFlags == 0 and NULL default-char pointers; with those
    // WideCharToMultiByte cannot fail on valid UTF-16, so no character ever
    // gets rejected the way the ANSI code page would reject it.
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                             static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    if (utf8.empty()) return {};

    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                             static_cast<int>(utf8.size()),
                                             nullptr, 0);
    if (needed <= 0) return {};

    std::wstring wide(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          wide.data(), needed);
    return std::filesystem::path(std::move(wide));
}

#else

std::string pathToUtf8(const std::filesystem::path& path)
{
    // u8string() is guaranteed UTF-8 and never lossy. Reinterpret the char8_t
    // bytes as char; the byte values are identical.
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

#endif

} // namespace mv
