#include "Themes.h"
#include "Config/ConfigSection.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include "Utils.h"

std::shared_ptr<AppTheme> CometDefaultThemes::DefaultLightMode = nullptr;
std::shared_ptr<AppTheme> CometDefaultThemes::DefaultDarkMode = nullptr;
std::vector<std::shared_ptr<AppTheme>> CometDefaultThemes::DefaultThemesList;

std::shared_ptr<VisualTheme> CometDefaultThemes::DefaultVisualTheme = nullptr;
std::shared_ptr<VisualTheme> CometDefaultThemes::SynthesiaVisualTheme = nullptr;
std::vector<std::shared_ptr<VisualTheme>> CometDefaultThemes::DefaultVisualThemesList;

void CometDefaultThemes::InitializeDefaultThemes()
{
    DefaultLightMode = std::make_shared<AppTheme>();
    auto& lmInfo = DefaultLightMode->info;
    lmInfo.name = "Comet light mode";
    lmInfo.author = "ponluxime";
    lmInfo.description = "Comet's default light mode colours.";
    lmInfo.version = 1;

    // we don't need to do anything with the default light mode

    DefaultDarkMode = std::make_shared<AppTheme>();
    auto& dmInfo = DefaultDarkMode->info;
    dmInfo.name = "Comet dark mode";
    dmInfo.author = "ponluxime";
    dmInfo.description = "Comet's default dark mode colours.";
    dmInfo.version = 1;

    // ... but we need to change everything with dark mode
    auto& dmColors = DefaultDarkMode->colors;
    dmColors.text = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);
    dmColors.textDisabled = ImVec4(0.85f, 0.85f, 0.85f, 0.50f);

    dmColors.background = ImVec4(0.22f, 0.22f, 0.22f, 1.00f); // Main window background
    dmColors.backgroundAlt = ImVec4(0.16f, 0.16f, 0.16f, 1.00f); // Child, Popup backgrounds
    dmColors.headerBg = ImVec4(0.12f, 0.12f, 0.12f, 1.00f); // Title bars, Menu bars

    dmColors.controlBase = ImVec4(0.35f, 0.35f, 0.35f, 1.00f); // Buttons, Inactive headers
    dmColors.controlHover = ImVec4(0.40f, 0.40f, 0.40f, 1.00f); // Hovered buttons/tabs
    dmColors.controlActive = ImVec4(0.45f, 0.45f, 0.45f, 1.00f); // Pressed buttons/active tabs

    dmColors.inputBg = ImVec4(0.12f, 0.12f, 0.12f, 1.00f); // Text input/checkbox backgrounds

    dmColors.accent = ImVec4(0.18f, 0.43f, 0.71f, 1.00f); // Checkmarks, slider grabs (Unity Blue)
    dmColors.accentActive = ImVec4(0.22f, 0.50f, 0.82f, 1.00f); // Active grabbers (Brighter blue)

    dmColors.border = ImVec4(0.10f, 0.10f, 0.10f, 1.00f); // Dark, crisp borders
    dmColors.separator = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);

    // --- Visual themes -----------------------------------------------------
    // The default keeps Comet's original rounded, roomy controls.
    DefaultVisualTheme = std::make_shared<VisualTheme>();
    auto& dvInfo = DefaultVisualTheme->info;
    dvInfo.name = "Comet default";
    dvInfo.author = "ponluxime";
    dvInfo.description = "Rounded controls and roomy spacing.";
    dvInfo.version = 1;

    // Synthesia's chrome, measured from Synthesia 10.9: flat rows with a light 1px outline,
    // 4px-rounded inset fields, compact padding, segmented tabs without an accent line, and an
    // opaque toolbar.
    SynthesiaVisualTheme = std::make_shared<VisualTheme>();
    auto& svInfo = SynthesiaVisualTheme->info;
    svInfo.name = "Synthesia REMASTERED";
    svInfo.author = "Systematics";
    svInfo.description = "Flat rows, light outlines, compact spacing.";
    svInfo.version = 1;

    auto& svStyle = SynthesiaVisualTheme->style;
    svStyle.windowRounding = 0.0f;
    svStyle.childRounding = 0.0f;
    svStyle.frameRounding = 4.0f;
    svStyle.popupRounding = 4.0f;
    svStyle.scrollbarRounding = 2.0f;
    svStyle.grabRounding = 3.0f;
    svStyle.tabRounding = 3.0f;

    svStyle.windowBorderSize = 0.0f;
    svStyle.frameBorderSize = 1.0f;
    svStyle.popupBorderSize = 1.0f;
    svStyle.tabBarBorderSize = 0.0f;
    svStyle.tabBarOverlineSize = 0.0f;

    svStyle.itemSpacing = ImVec2(8.0f, 6.0f);
    svStyle.windowPadding = ImVec2(12.0f, 9.0f);
    svStyle.framePadding = ImVec2(10.0f, 6.0f);
    svStyle.cellPadding = ImVec2(6.0f, 3.0f);
    svStyle.scrollbarSize = 12.0f;
    svStyle.grabMinSize = 10.0f;
    svStyle.framedToolbar = true;

    SynthesiaVisualTheme->synthesiaChrome = true;

    DefaultThemesList.push_back(DefaultLightMode);
    DefaultThemesList.push_back(DefaultDarkMode);

    DefaultVisualThemesList.push_back(DefaultVisualTheme);
    DefaultVisualThemesList.push_back(SynthesiaVisualTheme);
}

void AppTheme::ApplyColors()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    c[ImGuiCol_Text] = colors.text;
    c[ImGuiCol_TextDisabled] = colors.textDisabled;

    // Backgrounds
    c[ImGuiCol_WindowBg] = colors.background;
    c[ImGuiCol_ChildBg] = colors.backgroundAlt;
    c[ImGuiCol_PopupBg] = colors.backgroundAlt;
    c[ImGuiCol_DockingEmptyBg] = colors.background;

    // Title / Menu / Scrollbar bases
    c[ImGuiCol_TitleBg] = colors.headerBg;
    c[ImGuiCol_TitleBgCollapsed] = colors.headerBg;
    c[ImGuiCol_MenuBarBg] = colors.headerBg;
    c[ImGuiCol_ScrollbarBg] = colors.headerBg;

    // Borders
    c[ImGuiCol_Border] = colors.border;
    c[ImGuiCol_Separator] = colors.separator;

    // Inputs / Frames
    c[ImGuiCol_FrameBg] = colors.inputBg;
    c[ImGuiCol_FrameBgHovered] = colors.controlBase;
    c[ImGuiCol_FrameBgActive] = colors.controlHover;

    // Buttons
    c[ImGuiCol_Button] = colors.controlBase;
    c[ImGuiCol_ButtonHovered] = colors.controlHover;
    c[ImGuiCol_ButtonActive] = colors.controlActive;

    // Headers (Tree nodes, collapsibles)
    c[ImGuiCol_Header] = colors.controlBase;
    c[ImGuiCol_HeaderHovered] = colors.controlHover;
    c[ImGuiCol_HeaderActive] = colors.controlActive;

    // Tabs
    c[ImGuiCol_Tab] = colors.controlBase;
    c[ImGuiCol_TabHovered] = colors.controlHover;
    c[ImGuiCol_TabSelected] = colors.controlActive;
    c[ImGuiCol_TabDimmed] = colors.controlBase;
    c[ImGuiCol_TabDimmedSelected] = colors.controlHover;

    // Scrollbar Grabs
    c[ImGuiCol_ScrollbarGrab] = colors.controlActive;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(std::max(colors.controlActive.x - 0.1f, 0.0f), std::max(colors.controlActive.y - 0.1f, 0.0f), std::max(colors.controlActive.z - 0.1f, 0.0f), 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(std::max(colors.controlActive.x - 0.2f, 0.0f), std::max(colors.controlActive.y - 0.2f, 0.0f), std::max(colors.controlActive.z - 0.2f, 0.0f), 1.0f);

    // Accents
    c[ImGuiCol_CheckMark] = colors.accent;
    c[ImGuiCol_SliderGrab] = colors.accent;
    c[ImGuiCol_SliderGrabActive] = colors.accentActive;
    c[ImGuiCol_TabSelectedOverline] = colors.accent;
    c[ImGuiCol_PlotLines] = colors.accent;
    c[ImGuiCol_PlotHistogram] = colors.accent;

    // Resize Grips
    c[ImGuiCol_ResizeGrip] = ImVec4(colors.border.x, colors.border.y, colors.border.z, 0.30f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(colors.accent.x, colors.accent.y, colors.accent.z, 0.70f);
    c[ImGuiCol_ResizeGripActive] = colors.accent;

    // Checkboxes
    c[ImGuiCol_CheckboxSelectedBg] = colors.background;

    // Other
    c[ImGuiCol_TitleBgActive] = colors.controlBase;
    c[ImGuiCol_DockingPreview] = ImVec4(colors.accent.x, colors.accent.y, colors.accent.z, 0.25f);
}

void VisualTheme::Apply(const ThemeColors& colors) const
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = style.windowRounding;
    s.ChildRounding = style.childRounding;
    s.FrameRounding = style.frameRounding;
    s.PopupRounding = style.popupRounding;
    s.ScrollbarRounding = style.scrollbarRounding;
    s.GrabRounding = style.grabRounding;
    s.TabRounding = style.tabRounding;

    s.WindowBorderSize = style.windowBorderSize;
    s.FrameBorderSize = style.frameBorderSize;
    s.PopupBorderSize = style.popupBorderSize;
    s.TabBarBorderSize = style.tabBarBorderSize;
    s.TabBarOverlineSize = style.tabBarOverlineSize;

    s.ItemSpacing = style.itemSpacing;
    s.WindowPadding = style.windowPadding;
    s.FramePadding = style.framePadding;
    s.CellPadding = style.cellPadding;
    s.ScrollbarSize = style.scrollbarSize;
    s.GrabMinSize = style.grabMinSize;

    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    if (!synthesiaChrome) return;

    // Synthesia's outlines are light on its charcoal chrome. Deriving them from the active
    // palette keeps them readable on a light Comet theme too.
    const float luminance =
        colors.background.x * 0.2126f + colors.background.y * 0.7152f + colors.background.z * 0.0722f;
    const bool dark = luminance < 0.5f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Border] = dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.22f) : ImVec4(0.0f, 0.0f, 0.0f, 0.22f);
    c[ImGuiCol_Separator] = dark ? ImVec4(1.0f, 1.0f, 1.0f, 0.10f) : ImVec4(0.0f, 0.0f, 0.0f, 0.12f);

    // Synthesia's content panels are flush with the window, and its fields sit slightly darker
    // than the panel instead of being a separate bright surface.
    c[ImGuiCol_ChildBg] = colors.background;
    c[ImGuiCol_FrameBg] = colors.backgroundAlt;

    // The top strip and dialog headers share one slightly-lifted charcoal tone, as in Synthesia.
    c[ImGuiCol_MenuBarBg] = colors.backgroundAlt;
    c[ImGuiCol_TitleBg] = colors.backgroundAlt;
    c[ImGuiCol_TitleBgCollapsed] = colors.backgroundAlt;
    c[ImGuiCol_TitleBgActive] = colors.backgroundAlt;

    // Flat segmented tabs: unselected sit on the header strip, the selected one shares the
    // content background, and there is no bright overline.
    c[ImGuiCol_Tab] = colors.headerBg;
    c[ImGuiCol_TabHovered] = colors.controlHover;
    c[ImGuiCol_TabSelected] = colors.background;
    c[ImGuiCol_TabDimmed] = colors.headerBg;
    c[ImGuiCol_TabDimmedSelected] = colors.background;
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
}

ThemesList::ThemesList(const std::string& themesFolder)
{
    this->themesPath = themesFolder;

    if (!std::filesystem::exists(themesFolder))
    {
        std::filesystem::create_directory(themesFolder);
    }

    ReloadThemesList();
}

void ThemesList::SetTheme(size_t themeIdx)
{
    if (availableThemes.empty()) return;

    // A theme file may have been removed since this index was stored in the config.
    currentTheme = themeIdx >= availableThemes.size() ? availableThemes.size() - 1 : themeIdx;
}

void ThemesList::ApplyCurrent()
{
    if (availableThemes.empty()) return;
    GetCurrentTheme()->ApplyColors();
}

#define LOAD_THEME_COLOR(field, yamlLabel) themeColors.field = ParseColor(themeSec[yamlLabel], themeColors.field)

std::shared_ptr<AppTheme> ThemesList::ParseTheme(std::string path)
{
    std::optional<ConfigSection> theme;
    std::shared_ptr<AppTheme> parsedTheme = std::make_shared<AppTheme>();
    try
    {
        std::ifstream stream(path, std::ios::in);
        if (!stream.is_open())
        {
            std::cout << "Failed to open theme at path " << path << ". Returning default light mode theme." << std::endl;
            return CometDefaultThemes::DefaultLightMode;
        }

        YAML::Node themeYml = YAML::Load(stream);
        theme = ConfigSection(themeYml);
        std::optional<ConfigSection> infoSec = theme->GetSection("information");
        if (infoSec)
        {
            ThemeInfo themeInfo;
            themeInfo.author = infoSec->GetString("author", themeInfo.author);
            themeInfo.name = infoSec->GetString("name", themeInfo.name);
            themeInfo.description = infoSec->GetString("description", themeInfo.description);
            themeInfo.version = infoSec->GetInt("version", themeInfo.version);

            parsedTheme->info = std::move(themeInfo);
        }

        std::optional<ConfigSection> themeSec = theme->GetSection("theme");
        if (themeSec)
        {
            ThemeColors themeColors;
            YAML::Node themeSec = themeYml["theme"];

            LOAD_THEME_COLOR(text, "text");
            LOAD_THEME_COLOR(textDisabled, "textDisabled");

            LOAD_THEME_COLOR(background, "background");
            LOAD_THEME_COLOR(backgroundAlt, "backgroundAlt");
            LOAD_THEME_COLOR(headerBg, "headerBackground");

            LOAD_THEME_COLOR(controlBase, "controlBase");
            LOAD_THEME_COLOR(controlHover, "controlHover");
            LOAD_THEME_COLOR(controlActive, "controlActive");

            LOAD_THEME_COLOR(inputBg, "inputBackground");
            LOAD_THEME_COLOR(accent, "accent");
            LOAD_THEME_COLOR(accentActive, "accentActive");

            LOAD_THEME_COLOR(border, "borderColor");
            LOAD_THEME_COLOR(separator, "separatorColor");

            parsedTheme->colors = std::move(themeColors);
        }
    }
    catch (...)
    {
        std::cout << "Failed to parse theme at path " << path << "." << std::endl;
    }

    return parsedTheme;
}

void ThemesList::ReloadThemesList()
{
    availableThemes = CometDefaultThemes::DefaultThemesList;
    for (const auto& file : std::filesystem::directory_iterator(this->themesPath))
    {
        if (!file.is_regular_file()) continue;
        if (file.path().filename().extension() != ".yml") continue;
        std::shared_ptr<AppTheme> parsedTheme = ParseTheme(file.path().string());
        availableThemes.push_back(parsedTheme);
    }

    // The selected custom theme may have been deleted or renamed on disk.
    if (currentTheme >= availableThemes.size())
        currentTheme = availableThemes.empty() ? 0 : availableThemes.size() - 1;
}

ImVec4 ThemesList::ParseColor(const YAML::Node& node, const ImVec4& defaultColor)
{
    if (node && node.IsSequence() && node.size() >= 3)
    {
        float r = node[0].as<float>();
        float g = node[1].as<float>();
        float b = node[2].as<float>();
        float a = node.size() == 4 ? node[3].as<float>() : 1.0f;
        return ImVec4(r, g, b, a);
    }
    return defaultColor;
}

void ThemesList::OpenThemeListFolder() const
{
    Utils::OpenFolder(themesPath);
}

VisualThemeList::VisualThemeList()
{
    availableThemes = CometDefaultThemes::DefaultVisualThemesList;
    SetTheme(0);
}

void VisualThemeList::SetTheme(size_t themeIdx)
{
    if (availableThemes.empty()) return;
    currentTheme = themeIdx >= availableThemes.size() ? availableThemes.size() - 1 : themeIdx;
}

void VisualThemeList::ApplyCurrent(const ThemeColors& colors)
{
    if (availableThemes.empty()) return;
    GetCurrentTheme()->Apply(colors);
}
