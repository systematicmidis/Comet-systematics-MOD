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

struct ThemeInfo
{
    std::string name = "Unnamed theme";
    std::string author = "No author";
    std::string description = "No description set.";
    int version = 0;
};

// Palettes the custom-theme editor starts from. Synthesia REMASTERED follows the app's light or
// dark appearance the same way the Synthesia opening banner does: Crystal Blue on a light Comet
// theme, Synthesia Classic on a dark one.
namespace ThemePresets
{
    bool IsDark(const ThemeColors& colors);
    ThemeColors CometDefault(bool dark);
    ThemeColors SynthesiaRemastered(bool dark);
}

class AppTheme
{
public:
    AppTheme() = default;
    ~AppTheme() = default;

    void ApplyTheme();

    ThemeColors colors;
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
        if (availableThemes.empty()) return;

        // A config written while a custom theme existed can point past the end of the list.
        if (themeIdx >= availableThemes.size())
            themeIdx = availableThemes.size() - 1;

        currentTheme = themeIdx;
        availableThemes[currentTheme]->ApplyTheme();
    }
    void ReloadThemesList();
    void OpenThemeListFolder() const;

    // Writes the theme to <themes folder>/<name>.yml, reloads the list and selects the saved
    // theme. Returns false with an explanation in error when the file cannot be written.
    bool SaveTheme(const AppTheme& theme, std::string& error);
private:
    std::shared_ptr<AppTheme> ParseTheme(std::string path);
    ImVec4 ParseColor(const YAML::Node& node, const ImVec4& defaultColor);

    std::string themesPath;
    size_t currentTheme = 0;
    std::vector<std::shared_ptr<AppTheme>> availableThemes = CometDefaultThemes::DefaultThemesList;
};
