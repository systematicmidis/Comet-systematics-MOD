#pragma once

#include <vector>
#include <string>
#include <filesystem>

struct ImFont;
struct ImFontAtlas;

inline const char* FONT_FOLDER = "assets/fonts";

struct FontEntry
{
	std::string name = "Default Font";
	std::string path = "";
	ImFont* font = nullptr;
};

// True when two font paths name the same file. The comparison tolerates different separators,
// relative vs absolute paths and, on Windows, letter case, so a font chosen through the settings
// dropdown keeps matching after the path has been written to and read back from config.yml.
bool FontPathsMatch(const std::string& a, const std::string& b);

class FontList
{
public:
	FontList();
	void ReloadList();
	void ValidateFontList();

	const FontEntry& GetCurrentFont() const
	{
		if (fonts.empty() || currentFont >= fonts.size())
		{
			return defaultFontEntry;
		}
		return fonts[currentFont];
	}

	void SetFont(size_t fontIdx)
	{
		currentFont = fontIdx;
		if (currentFont >= fonts.size() && !fonts.empty())
			currentFont = fonts.size() - 1;
	}

	size_t GetCurrentFontID() const { return currentFont; }
	std::vector<FontEntry>& GetFonts() { return fonts; }

	// The entry for a selected font path, or null when the list has no match. The path comparison
	// is normalized (see FontPathsMatch), so it does not depend on separators or path form.
	const FontEntry* FindFont(const std::string& path) const
	{
		if (path.empty()) return nullptr;

		for (const FontEntry& entry : fonts)
		{
			if (FontPathsMatch(entry.path, path))
				return &entry;
		}

		return nullptr;
	}

	// The loaded font for a selected path, or null when it is not in the list or failed to load.
	// The result is cached because this is called every frame by the overlays and banner.
	ImFont* GetFontForPath(const std::string& path) const
	{
		if (cachedFontValid && cachedFontPath == path)
			return cachedFont;

		const FontEntry* entry = FindFont(path);
		cachedFontPath = path;
		cachedFont = entry != nullptr ? entry->font : nullptr;
		cachedFontValid = true;
		return cachedFont;
	}

	// must be called before ImGui builds its font atlas (typically during early startup)
	void RegisterFontsIntoAtlas(ImFontAtlas* atlas, float defaultFontSize = 18.0f);

private:
	std::vector<FontEntry> fonts{};
	size_t currentFont = 0;
	FontEntry defaultFontEntry{};

	mutable std::string cachedFontPath{};
	mutable ImFont* cachedFont = nullptr;
	mutable bool cachedFontValid = false;

	void CreateDefaultFontsFolder(bool makeFolder = true);
};