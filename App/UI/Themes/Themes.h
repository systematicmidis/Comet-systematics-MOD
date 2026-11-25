#pragma once
#include <yaml-cpp/yaml.h>
#include <imgui.h>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>

class AppTheme;

namespace CometDefaultThemes
{
    extern std::shared_ptr<AppTheme> DefaultLightMode;
    extern std::shared_ptr<AppTheme> DefaultDarkMode;
    // Built-in recreation of Synthesia's dark UI. Like the two Comet themes it lives in the binary
    // and is never written to the themes folder; that folder only holds themes the user creates.
    extern std::shared_ptr<AppTheme> SynthesiaRemastered;
    extern std::vector<std::shared_ptr<AppTheme>> DefaultThemesList;
    void InitializeDefaultThemes();
}

struct ThemeColors
{
    ImVec4 text =          ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    ImVec4 textDisabled =  ImVec4(0.10f, 0.10f, 0.10f, 0.50f);

    ImVec4 background =    ImVec4(0.94f, 0.94f, 0.94f, 1.00f); // Window background
    ImVec4 backgroundAlt = ImVec4(0.97f, 0.97f, 0.97f, 1.00f); // Child, Popup backgrounds
    ImVec4 headerBg =      ImVec4(0.92f, 0.92f, 0.92f, 1.00f); // Title bars, Menu bars, Scrollbars

    ImVec4 controlBase =   ImVec4(0.90f, 0.90f, 0.90f, 1.00f); // Buttons, Inactive headers
    ImVec4 controlHover =  ImVec4(0.82f, 0.82f, 0.82f, 1.00f); // Hovered buttons/tabs
    ImVec4 controlActive = ImVec4(0.75f, 0.75f, 0.75f, 1.00f); // Pressed buttons/active tabs

    ImVec4 inputBg =       ImVec4(1.00f, 1.00f, 1.00f, 1.00f); // Text input/checkbox backgrounds

    ImVec4 accent =        ImVec4(0.20f, 0.45f, 0.90f, 1.00f); // Checkmarks, slider grabs, plot lines
    ImVec4 accentActive =  ImVec4(0.15f, 0.40f, 0.85f, 1.00f); // Active grabbers

    ImVec4 border =        ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    ImVec4 separator =     ImVec4(0.75f, 0.75f, 0.75f, 1.00f);
};

// Shape and spacing of the UI, so a theme can change how controls look and not only their
// colours. The defaults match Comet's original layout. Custom theme files can override any of
// these in a "style" section (see Themes.cpp).
struct ThemeStyle
{
    float windowRounding = 6.0f;
    float childRounding = 4.0f;
    float frameRounding = 4.0f;
    float popupRounding = 4.0f;
    float scrollbarRounding = 6.0f;
    float grabRounding = 4.0f;

    float windowBorderSize = 1.0f;
    float frameBorderSize = 1.0f;
    float popupBorderSize = 1.0f;

    ImVec2 itemSpacing = ImVec2(8.0f, 6.0f);
    ImVec2 windowPadding = ImVec2(10.0f, 10.0f);
    ImVec2 framePadding = ImVec2(4.0f, 3.0f);
};

struct ThemeInfo
{
    std::string name = "Unnamed theme";
    std::string author = "No author";
    std::string description = "No description set.";
    int version = 0;
};

class AppTheme
{
public:
    AppTheme() = default;
    ~AppTheme() = default;

    void ApplyTheme();

    ThemeColors colors;
    ThemeStyle style;
    ThemeInfo info;
};

class ThemesList
{
public:
    ThemesList(const std::string& themesFolder);
    std::vector<std::shared_ptr<AppTheme>>& GetThemesList()
    {
        return availableThemes;
    }
    AppTheme* GetCurrentTheme()
    {
        return availableThemes[currentTheme].get();
    }
    size_t GetCurrentThemeIndex() const
    {
        return currentTheme;
    }
    void SetThemeAndApply(size_t themeIdx)
    {
        if (availableThemes.size() == 0) return;

        // A theme file may have been removed since this index was stored in the config.
        if (themeIdx >= availableThemes.size())
        {
            SetThemeAndApply(availableThemes.size() - 1);
            return;
        }
        currentTheme = themeIdx;
        GetCurrentTheme()->ApplyTheme();
    }
    void ReloadThemesList();
    void OpenThemeListFolder() const;
private:
    std::shared_ptr<AppTheme> ParseTheme(std::string path);
    ImVec4 ParseColor(const YAML::Node& node, const ImVec4& defaultColor);

    std::string themesPath;
    size_t currentTheme = 0;
    std::vector<std::shared_ptr<AppTheme>> availableThemes = CometDefaultThemes::DefaultThemesList;
};
