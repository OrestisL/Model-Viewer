#pragma once

#include <imgui.h>

namespace mv::ui {

enum class ThemeStyle : int
{
    Dark = 0,
    Light,
    Midnight,   ///< Near-black, for working against bright models
    Count
};

const char* themeStyleName(ThemeStyle style);

/// Applies colours, spacing and rounding.
///
/// `accent` drives every interactive colour -- checkmarks, sliders, selected
/// tabs, resize grips -- so the interface reads as one thing rather than a
/// collection of separately chosen blues. Hovered and active variants are
/// derived from it rather than listed, which is what keeps them consistent.
///
/// `uiScale` multiplies all metrics. The style is rebuilt from scratch each
/// call because ScaleAllSizes compounds if applied to an already-scaled style.
void applyStyle(ThemeStyle style, const ImVec4& accent, float uiScale);

/// Loads a UI font from the system.
///
/// ImGui's built-in font is a 13px bitmap from 2005, and no amount of colour
/// work hides it -- it is the single thing that makes an ImGui application
/// look dated. A system font also inherits whatever the platform considers
/// readable, which is closer to "native" than any palette.
///
/// Must be called before the backend uploads the font atlas, i.e. before
/// ImGui_ImplVulkan_Init. Falls back to the built-in font if nothing is found.
/// Returns the path used, or an empty string.
const char* loadUiFont(float sizePixels);

} // namespace mv::ui
