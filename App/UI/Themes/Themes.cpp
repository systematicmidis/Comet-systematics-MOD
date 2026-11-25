#include "Themes.h"
#include "Config/ConfigSection.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include "Utils.h"

std::shared_ptr<AppTheme> CometDefaultThemes::DefaultLightMode = nullptr;
std::shared_ptr<AppTheme> CometDefaultThemes::DefaultDarkMode = nullptr;
std::shared_ptr<AppTheme> CometDefaultThemes::SynthesiaRemastered = nullptr;
std::vector<std::shared_ptr<AppTheme>> CometDefaultThemes::DefaultThemesList;

void CometDefaultThemes::InitializeDefaultThemes()
{
	DefaultLightMode = std::make_shared<AppTheme>();
	auto& lmInfo = DefaultLightMode->info;
	lmInfo.name = "Comet light mode";
	lmInfo.author = "ponluxime";
	lmInfo.description = "Comet's default light mode theme.";
	lmInfo.version = 1;

	// we don't need to do anything with the default light mode

	DefaultDarkMode = std::make_shared<AppTheme>();
	auto& dmInfo = DefaultDarkMode->info;
	dmInfo.name = "Comet dark mode";
	dmInfo.author = "ponluxime";
	dmInfo.description = "Comet's default dark mode theme.";
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

	// Built-in Synthesia visual theme: Synthesia's dark chrome with flat, square controls and its
	// blue accent. The shape values are what make it read as Synthesia rather than a recolour.
	SynthesiaRemastered = std::make_shared<AppTheme>();
	auto& srInfo = SynthesiaRemastered->info;
	srInfo.name = "Synthesia REMASTERED";
	srInfo.author = "Systematics";
	srInfo.description = "Synthesia's dark UI: flat square controls, charcoal panels and its blue accent.";
	srInfo.version = 1;

	auto& srColors = SynthesiaRemastered->colors;
	srColors.text = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
	srColors.textDisabled = ImVec4(0.90f, 0.92f, 0.96f, 0.45f);
	srColors.background = ImVec4(0.14f, 0.16f, 0.18f, 1.00f); // #24282E
	srColors.backgroundAlt = ImVec4(0.10f, 0.11f, 0.13f, 1.00f); // #1A1C21
	srColors.headerBg = ImVec4(0.07f, 0.08f, 0.09f, 1.00f); // #121417
	srColors.controlBase = ImVec4(0.25f, 0.27f, 0.33f, 1.00f); // #3F4654
	srColors.controlHover = ImVec4(0.30f, 0.33f, 0.40f, 1.00f);
	srColors.controlActive = ImVec4(0.36f, 0.39f, 0.47f, 1.00f);
	srColors.inputBg = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
	srColors.accent = ImVec4(0.30f, 0.58f, 0.88f, 1.00f); // #4C94E0
	srColors.accentActive = ImVec4(0.00f, 0.66f, 1.00f, 1.00f); // #00A8FF
	srColors.border = ImVec4(0.05f, 0.05f, 0.06f, 1.00f);
	srColors.separator = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);

	auto& srStyle = SynthesiaRemastered->style;
	srStyle.windowRounding = 0.0f;
	srStyle.childRounding = 0.0f;
	srStyle.frameRounding = 0.0f;
	srStyle.popupRounding = 0.0f;
	srStyle.scrollbarRounding = 0.0f;
	srStyle.grabRounding = 0.0f;
	srStyle.itemSpacing = ImVec2(7.0f, 5.0f);
	srStyle.windowPadding = ImVec2(9.0f, 8.0f);
	srStyle.framePadding = ImVec2(8.0f, 5.0f);

    DefaultThemesList.push_back(DefaultLightMode);
    DefaultThemesList.push_back(DefaultDarkMode);
    DefaultThemesList.push_back(SynthesiaRemastered);
}

void AppTheme::ApplyTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();

    // -----------------------
    // Layout / feel
    // -----------------------
    style.WindowRounding = this->style.windowRounding;
    style.ChildRounding = this->style.childRounding;
    style.FrameRounding = this->style.frameRounding;
    style.PopupRounding = this->style.popupRounding;
    style.ScrollbarRounding = this->style.scrollbarRounding;
    style.GrabRounding = this->style.grabRounding;

    style.WindowBorderSize = this->style.windowBorderSize;
    style.FrameBorderSize = this->style.frameBorderSize;
    style.PopupBorderSize = this->style.popupBorderSize;

    style.ItemSpacing = this->style.itemSpacing;
    style.WindowPadding = this->style.windowPadding;
    style.FramePadding = this->style.framePadding;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    // -----------------------
    // Colors
    // -----------------------
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

ThemesList::ThemesList(const std::string& themesFolder)
{
    this->themesPath = themesFolder;
    
    if (!std::filesystem::exists(themesFolder))
    {
        std::filesystem::create_directory(themesFolder);
    }

    ReloadThemesList();
}

#define LOAD_THEME_COLOR(field, yamlLabel) themeColors.field = ParseColor(themeSec[yamlLabel], themeColors.field)

static ImVec2 ParseThemeVec2(const YAML::Node& node, const ImVec2& defaultVec)
{
    if (node && node.IsSequence() && node.size() >= 2)
        return ImVec2(node[0].as<float>(), node[1].as<float>());

    return defaultVec;
}

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

        // Optional UI shape. Older theme files simply leave every value at Comet's defaults.
        std::optional<ConfigSection> styleSec = theme->GetSection("style");
        if (styleSec)
        {
            YAML::Node styleNode = themeYml["style"];
            ThemeStyle themeStyle;

            themeStyle.windowRounding = styleSec->GetFloat("windowRounding", themeStyle.windowRounding);
            themeStyle.childRounding = styleSec->GetFloat("childRounding", themeStyle.childRounding);
            themeStyle.frameRounding = styleSec->GetFloat("frameRounding", themeStyle.frameRounding);
            themeStyle.popupRounding = styleSec->GetFloat("popupRounding", themeStyle.popupRounding);
            themeStyle.scrollbarRounding = styleSec->GetFloat("scrollbarRounding", themeStyle.scrollbarRounding);
            themeStyle.grabRounding = styleSec->GetFloat("grabRounding", themeStyle.grabRounding);

            themeStyle.windowBorderSize = styleSec->GetFloat("windowBorderSize", themeStyle.windowBorderSize);
            themeStyle.frameBorderSize = styleSec->GetFloat("frameBorderSize", themeStyle.frameBorderSize);
            themeStyle.popupBorderSize = styleSec->GetFloat("popupBorderSize", themeStyle.popupBorderSize);

            themeStyle.itemSpacing = ParseThemeVec2(styleNode["itemSpacing"], themeStyle.itemSpacing);
            themeStyle.windowPadding = ParseThemeVec2(styleNode["windowPadding"], themeStyle.windowPadding);
            themeStyle.framePadding = ParseThemeVec2(styleNode["framePadding"], themeStyle.framePadding);

            parsedTheme->style = themeStyle;
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
