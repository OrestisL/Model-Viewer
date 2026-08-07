#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mv {

/// A small ImGui file dialog. Deliberately dependency-free so the project
/// builds on Linux and Windows without GTK/portal or COM plumbing.
class FileBrowser
{
public:
    enum class Mode { Open, Save };

    void open(Mode                            mode,
              std::string                     title,
              std::vector<std::string>        extensions,
              const std::filesystem::path&    startDirectory = {},
              const std::string&              suggestedName  = {});

    void close();
    bool isOpen() const { return m_open; }

    /// Draws the modal. Returns true exactly once, on confirmation.
    bool draw();

    const std::filesystem::path& result() const { return m_result; }

private:
    void refresh();

    struct Entry
    {
        std::string           label;
        std::filesystem::path path;
        bool                  isDirectory = false;
        uintmax_t             size        = 0;
    };

    bool                     m_open = false;
    Mode                     m_mode = Mode::Open;
    std::string              m_title = "Open file";
    std::vector<std::string> m_extensions;      // lower-case, no dots; empty = all

    std::filesystem::path m_directory;
    std::filesystem::path m_result;
    std::vector<Entry>    m_entries;
    std::string           m_error;

    char m_filenameBuffer[512]{};
    int  m_selected = -1;
    bool m_showHidden = false;
    bool m_needsRefresh = true;
};

} // namespace mv
