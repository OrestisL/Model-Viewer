#pragma once

#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace mv::log {

enum class Level { Trace, Info, Warn, Error };

struct Entry
{
    Level       level;
    std::string message;
};

class Sink
{
public:
    static Sink& get()
    {
        static Sink instance;
        return instance;
    }

    void push(Level level, std::string message)
    {
        const char* tag = "info ";
        FILE* stream = stdout;
        switch (level)
        {
            case Level::Trace: tag = "trace"; break;
            case Level::Info:  tag = "info "; break;
            case Level::Warn:  tag = "warn "; stream = stderr; break;
            case Level::Error: tag = "error"; stream = stderr; break;
        }
        std::fprintf(stream, "[%s] %s\n", tag, message.c_str());
        std::fflush(stream);

        std::scoped_lock lock(m_mutex);
        m_entries.push_back({level, std::move(message)});
        while (m_entries.size() > kCapacity)
            m_entries.pop_front();
        ++m_revision;
    }

    /// Snapshot for the UI. Cheap enough at this scale.
    std::deque<Entry> snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_entries;
    }

    uint64_t revision() const
    {
        std::scoped_lock lock(m_mutex);
        return m_revision;
    }

    void clear()
    {
        std::scoped_lock lock(m_mutex);
        m_entries.clear();
        ++m_revision;
    }

private:
    static constexpr size_t kCapacity = 512;

    mutable std::mutex m_mutex;
    std::deque<Entry>  m_entries;
    uint64_t           m_revision = 0;
};

namespace detail {
template <typename... Args>
inline std::string join(Args&&... args)
{
    std::ostringstream oss;
    (oss << ... << args);
    return oss.str();
}
} // namespace detail

template <typename... Args> inline void trace(Args&&... a) { Sink::get().push(Level::Trace, detail::join(a...)); }
template <typename... Args> inline void info (Args&&... a) { Sink::get().push(Level::Info,  detail::join(a...)); }
template <typename... Args> inline void warn (Args&&... a) { Sink::get().push(Level::Warn,  detail::join(a...)); }
template <typename... Args> inline void error(Args&&... a) { Sink::get().push(Level::Error, detail::join(a...)); }

} // namespace mv::log
