#include "ui/Theme.hpp"

#include <cstdio>
#include <filesystem>

namespace mv::ui {
namespace {

/// Blends towards white (t > 0) or black (t < 0), keeping alpha.
ImVec4 shade(const ImVec4& c, float t)
{
    const float target = t > 0.0f ? 1.0f : 0.0f;
    const float k      = t > 0.0f ? t : -t;
    return ImVec4(c.x + (target - c.x) * k,
                  c.y + (target - c.y) * k,
                  c.z + (target - c.z) * k,
                  c.w);
}

ImVec4 withAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

struct Palette
{
    ImVec4 windowBg, childBg, popupBg, frameBg, border, text, textDim, header;
};

Palette paletteFor(ThemeStyle style)
{
    switch (style)
    {
        case ThemeStyle::Light:
            return {ImVec4(0.94f, 0.94f, 0.95f, 1.00f),
                    ImVec4(0.97f, 0.97f, 0.98f, 1.00f),
                    ImVec4(0.98f, 0.98f, 0.99f, 1.00f),
                    ImVec4(0.88f, 0.88f, 0.90f, 1.00f),
                    ImVec4(0.76f, 0.76f, 0.79f, 1.00f),
                    ImVec4(0.10f, 0.11f, 0.13f, 1.00f),
                    ImVec4(0.42f, 0.44f, 0.48f, 1.00f),
                    ImVec4(0.86f, 0.87f, 0.89f, 1.00f)};

        case ThemeStyle::Midnight:
            return {ImVec4(0.055f, 0.058f, 0.067f, 1.00f),
                    ImVec4(0.075f, 0.078f, 0.090f, 1.00f),
                    ImVec4(0.085f, 0.090f, 0.105f, 1.00f),
                    ImVec4(0.125f, 0.132f, 0.152f, 1.00f),
                    ImVec4(0.185f, 0.195f, 0.220f, 1.00f),
                    ImVec4(0.900f, 0.910f, 0.930f, 1.00f),
                    ImVec4(0.470f, 0.490f, 0.540f, 1.00f),
                    ImVec4(0.150f, 0.158f, 0.182f, 1.00f)};

        case ThemeStyle::Dark:
        default:
            return {ImVec4(0.105f, 0.110f, 0.125f, 1.00f),
                    ImVec4(0.128f, 0.134f, 0.152f, 1.00f),
                    ImVec4(0.140f, 0.147f, 0.166f, 1.00f),
                    ImVec4(0.170f, 0.178f, 0.202f, 1.00f),
                    ImVec4(0.235f, 0.245f, 0.275f, 1.00f),
                    ImVec4(0.902f, 0.910f, 0.925f, 1.00f),
                    ImVec4(0.500f, 0.520f, 0.570f, 1.00f),
                    ImVec4(0.195f, 0.205f, 0.235f, 1.00f)};
    }
}

} // namespace

const char* themeStyleName(ThemeStyle style)
{
    switch (style)
    {
        case ThemeStyle::Dark:     return "Dark";
        case ThemeStyle::Light:    return "Light";
        case ThemeStyle::Midnight: return "Midnight";
        default:                   return "unknown";
    }
}

void applyStyle(ThemeStyle style, const ImVec4& accent, float uiScale)
{
    const Palette p     = paletteFor(style);
    const bool    light = style == ThemeStyle::Light;

    // Start from a known state: this function is called again whenever the
    // theme or scale changes, and metrics must not accumulate.
    ImGuiStyle fresh;
    ImGui::GetStyle() = fresh;
    ImGuiStyle& s = ImGui::GetStyle();

    // Restrained rounding. Fully rounded controls read as a toy; square ones
    // read as 2010. A few pixels is what current desktop UI actually uses.
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 5.0f;
    s.ScrollbarRounding = 9.0f;

    // Hairline borders rather than none: they separate panels without the
    // heavy chrome that makes a dense UI feel cluttered.
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.PopupBorderSize  = 1.0f;
    s.ChildBorderSize  = 1.0f;
    s.TabBarBorderSize = 1.0f;

    // Generous spacing is most of what separates a modern interface from a
    // cramped one. ImGui's defaults are tuned for debug overlays, not tools.
    s.WindowPadding     = ImVec2(12, 12);
    s.FramePadding      = ImVec2(10, 6);
    s.CellPadding       = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 13.0f;
    s.GrabMinSize       = 11.0f;
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding    = ImVec2(18, 6);

    s.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;   // no collapse arrow clutter
    s.DockingSeparatorSize = 2.0f;

    ImVec4* c = s.Colors;

    c[ImGuiCol_Text]                 = p.text;
    c[ImGuiCol_TextDisabled]         = p.textDim;
    c[ImGuiCol_WindowBg]             = p.windowBg;
    c[ImGuiCol_ChildBg]              = p.childBg;
    c[ImGuiCol_PopupBg]              = p.popupBg;
    c[ImGuiCol_Border]               = p.border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg]              = p.frameBg;
    c[ImGuiCol_FrameBgHovered]       = shade(p.frameBg, light ? -0.06f : 0.08f);
    c[ImGuiCol_FrameBgActive]        = shade(p.frameBg, light ? -0.12f : 0.14f);

    c[ImGuiCol_TitleBg]              = p.windowBg;
    c[ImGuiCol_TitleBgActive]        = p.header;
    c[ImGuiCol_TitleBgCollapsed]     = withAlpha(p.windowBg, 0.75f);
    c[ImGuiCol_MenuBarBg]            = shade(p.windowBg, light ? -0.03f : 0.03f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = shade(p.frameBg, light ? -0.15f : 0.12f);
    c[ImGuiCol_ScrollbarGrabHovered] = shade(p.frameBg, light ? -0.25f : 0.22f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;

    // Everything interactive derives from the accent, so one setting retints
    // the whole interface coherently.
    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = shade(accent, 0.20f);

    c[ImGuiCol_Button]               = withAlpha(accent, light ? 0.20f : 0.24f);
    c[ImGuiCol_ButtonHovered]        = withAlpha(accent, 0.45f);
    c[ImGuiCol_ButtonActive]         = accent;

    c[ImGuiCol_Header]               = withAlpha(accent, light ? 0.18f : 0.22f);
    c[ImGuiCol_HeaderHovered]        = withAlpha(accent, 0.38f);
    c[ImGuiCol_HeaderActive]         = withAlpha(accent, 0.55f);

    c[ImGuiCol_Separator]            = p.border;
    c[ImGuiCol_SeparatorHovered]     = withAlpha(accent, 0.60f);
    c[ImGuiCol_SeparatorActive]      = accent;

    c[ImGuiCol_ResizeGrip]           = withAlpha(accent, 0.22f);
    c[ImGuiCol_ResizeGripHovered]    = withAlpha(accent, 0.50f);
    c[ImGuiCol_ResizeGripActive]     = accent;

    c[ImGuiCol_Tab]                  = shade(p.windowBg, light ? -0.04f : 0.03f);
    c[ImGuiCol_TabHovered]           = withAlpha(accent, 0.40f);
    c[ImGuiCol_TabSelected]          = p.header;
    c[ImGuiCol_TabSelectedOverline]  = accent;
    c[ImGuiCol_TabDimmed]            = p.windowBg;
    c[ImGuiCol_TabDimmedSelected]    = shade(p.header, light ? 0.03f : -0.03f);
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.35f);

    c[ImGuiCol_DockingPreview]       = withAlpha(accent, 0.45f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0, 0, 0, 0);   // the 3D view shows through

    c[ImGuiCol_PlotLines]            = shade(accent, 0.30f);
    c[ImGuiCol_PlotLinesHovered]     = accent;
    c[ImGuiCol_PlotHistogram]        = accent;
    c[ImGuiCol_PlotHistogramHovered] = shade(accent, 0.25f);

    c[ImGuiCol_TableHeaderBg]        = p.header;
    c[ImGuiCol_TableBorderStrong]    = p.border;
    c[ImGuiCol_TableBorderLight]     = withAlpha(p.border, 0.55f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = withAlpha(shade(p.windowBg, light ? -0.02f : 0.03f), 0.55f);

    c[ImGuiCol_TextSelectedBg]       = withAlpha(accent, 0.35f);
    c[ImGuiCol_TextLink]             = accent;
    c[ImGuiCol_NavCursor]            = accent;
    c[ImGuiCol_DragDropTarget]       = accent;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

    if (uiScale != 1.0f) s.ScaleAllSizes(uiScale);
}

const char* loadUiFont(float sizePixels)
{
    namespace fs = std::filesystem;

    // Ordered by preference: each platform's actual UI font first, then
    // whatever is likely to exist.
    static const char* const kCandidates[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/cantarell/Cantarell-VF.otf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/System/Library/Fonts/SFNS.ttf",
#endif
    };

    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig config;
    config.OversampleH = 2;    // crisper at small sizes without a huge atlas
    config.OversampleV = 1;
    config.PixelSnapH  = true;

    std::error_code ec;
    for (const char* path : kCandidates)
    {
        if (!fs::exists(path, ec)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, sizePixels, &config)) return path;
    }

    io.Fonts->AddFontDefault();
    return "";
}

} // namespace mv::ui
