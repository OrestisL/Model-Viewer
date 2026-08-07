#include "ui/FileBrowser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>

namespace fs = std::filesystem;

namespace mv {
namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string humanSize(uintmax_t bytes)
{
    const char* units[]{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int    unit  = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), (unit == 0) ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

fs::path defaultDirectory()
{
    std::error_code ec;

#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"))
        if (fs::is_directory(profile, ec)) return profile;
#else
    if (const char* home = std::getenv("HOME"))
        if (fs::is_directory(home, ec)) return home;
#endif

    return fs::current_path(ec);
}

} // namespace

void FileBrowser::open(Mode                 mode,
                       std::string          title,
                       std::vector<std::string> extensions,
                       const fs::path&      startDirectory,
                       const std::string&   suggestedName)
{
    m_mode       = mode;
    m_title      = std::move(title);
    m_extensions = std::move(extensions);
    for (std::string& extension : m_extensions)
        extension = lower(extension);

    std::error_code ec;
    m_directory = (!startDirectory.empty() && fs::is_directory(startDirectory, ec))
                      ? startDirectory
                      : defaultDirectory();

    std::snprintf(m_filenameBuffer, sizeof(m_filenameBuffer), "%s", suggestedName.c_str());

    m_selected     = -1;
    m_error.clear();
    m_result.clear();
    m_needsRefresh = true;
    m_open         = true;
}

void FileBrowser::close()
{
    m_open = false;
    ImGui::CloseCurrentPopup();
}

void FileBrowser::refresh()
{
    m_entries.clear();
    m_selected = -1;
    m_error.clear();

    std::error_code ec;

    if (m_directory.has_parent_path() && m_directory.parent_path() != m_directory)
        m_entries.push_back({"..", m_directory.parent_path(), true, 0});

    fs::directory_iterator it(m_directory, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        m_error = "Cannot read " + m_directory.string() + ": " + ec.message();
        return;
    }

    std::vector<Entry> directories, files;

    for (const fs::directory_entry& entry : it)
    {
        const std::string name = entry.path().filename().string();
        if (name.empty()) continue;
        if (!m_showHidden && name.front() == '.') continue;

        std::error_code entryEc;
        if (entry.is_directory(entryEc))
        {
            directories.push_back({"[ " + name + " ]", entry.path(), true, 0});
            continue;
        }

        if (!m_extensions.empty())
        {
            const std::string extension =
                entry.path().has_extension() ? lower(entry.path().extension().string().substr(1)) : "";
            if (std::find(m_extensions.begin(), m_extensions.end(), extension) == m_extensions.end())
                continue;
        }

        files.push_back({name, entry.path(), false, entry.file_size(entryEc)});
    }

    auto byLabel = [](const Entry& a, const Entry& b) { return lower(a.label) < lower(b.label); };
    std::sort(directories.begin(), directories.end(), byLabel);
    std::sort(files.begin(), files.end(), byLabel);

    m_entries.insert(m_entries.end(), directories.begin(), directories.end());
    m_entries.insert(m_entries.end(), files.begin(), files.end());
}

bool FileBrowser::draw()
{
    if (!m_open) return false;

    if (m_needsRefresh)
    {
        refresh();
        m_needsRefresh = false;
    }

    const char* popupId = "##file_browser";
    ImGui::OpenPopup(popupId);

    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    bool confirmed = false;

    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoSavedSettings))
        return false;

    ImGui::TextUnformatted(m_title.c_str());
    ImGui::Separator();

    // -- location bar ------------------------------------------------------
    if (ImGui::Button("Up") && m_directory.has_parent_path())
    {
        m_directory    = m_directory.parent_path();
        m_needsRefresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Home"))
    {
        m_directory    = defaultDirectory();
        m_needsRefresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Hidden", &m_showHidden)) m_needsRefresh = true;

    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_directory.string().c_str());

#if defined(_WIN32)
    ImGui::TextUnformatted("Drives:");
    std::error_code driveEc;
    for (char letter = 'A'; letter <= 'Z'; ++letter)
    {
        const fs::path drive = std::string(1, letter) + ":\\";
        if (!fs::exists(drive, driveEc)) continue;

        ImGui::SameLine();
        if (ImGui::SmallButton(std::string(1, letter).c_str()))
        {
            m_directory    = drive;
            m_needsRefresh = true;
        }
    }
#endif

    if (!m_error.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_error.c_str());

    // -- listing -----------------------------------------------------------
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.4f;
    if (ImGui::BeginChild("##entries", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders))
    {
        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            const Entry& entry = m_entries[static_cast<size_t>(i)];

            if (ImGui::Selectable(entry.label.c_str(), m_selected == i,
                                  ImGuiSelectableFlags_AllowDoubleClick))
            {
                m_selected = i;

                if (!entry.isDirectory)
                    std::snprintf(m_filenameBuffer, sizeof(m_filenameBuffer), "%s",
                                  entry.path.filename().string().c_str());

                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    if (entry.isDirectory)
                    {
                        m_directory    = entry.path;
                        m_needsRefresh = true;
                        break;
                    }

                    m_result  = entry.path;
                    confirmed = true;
                }
            }

            if (!entry.isDirectory)
            {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
                ImGui::TextDisabled("%s", humanSize(entry.size).c_str());
            }
        }
    }
    ImGui::EndChild();

    // -- footer ------------------------------------------------------------
    ImGui::SetNextItemWidth(-220.0f);
    const bool submitted = ImGui::InputText("##filename", m_filenameBuffer,
                                            sizeof(m_filenameBuffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SameLine();
    const bool accept = ImGui::Button(m_mode == Mode::Save ? "Save" : "Open", ImVec2(90, 0));

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        close();
        ImGui::EndPopup();
        return false;
    }

    if ((accept || submitted) && m_filenameBuffer[0] != '\0')
    {
        const fs::path candidate = m_directory / m_filenameBuffer;

        std::error_code ec;
        if (fs::is_directory(candidate, ec))
        {
            m_directory      = candidate;
            m_needsRefresh   = true;
            m_filenameBuffer[0] = '\0';
        }
        else if (m_mode == Mode::Open && !fs::exists(candidate, ec))
        {
            m_error = "No such file: " + candidate.string();
        }
        else
        {
            m_result  = candidate;
            confirmed = true;
        }
    }

    if (confirmed) close();

    ImGui::EndPopup();
    return confirmed;
}

} // namespace mv
