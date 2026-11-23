#include "FontList.h"
#include "Utils.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <iostream>

bool FontPathsMatch(const std::string& a, const std::string& b)
{
	if (a == b) return true;
	if (a.empty() || b.empty()) return false;

	auto normalize = [](const std::string& value)
	{
		std::error_code absoluteError;
		std::filesystem::path path = std::filesystem::absolute(value, absoluteError);
		if (absoluteError) path = value;

		std::string text = path.lexically_normal().generic_string();
#ifdef _WIN32
		std::transform(text.begin(), text.end(), text.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
#endif
		return text;
	};

	return normalize(a) == normalize(b);
}

FontList::FontList()
{
	ReloadList();
}

void FontList::ReloadList()
{
	cachedFontValid = false;
	ValidateFontList();
	fonts.clear();

	if (!std::filesystem::exists(FONT_FOLDER)) return;

	for (const auto& entry : std::filesystem::directory_iterator(FONT_FOLDER))
	{
		if (!entry.is_regular_file()) continue;
		std::string ext = entry.path().extension().string();
		if (ext != ".ttf" && ext != ".otf") continue;

		FontEntry fontEntry;
		fontEntry.name = entry.path().stem().string();
		fontEntry.path = entry.path().string();
		fontEntry.font = nullptr;

		fonts.push_back(fontEntry);
	}

	if (currentFont >= fonts.size() && !fonts.empty())
		currentFont = fonts.size() - 1;
}

void FontList::ValidateFontList()
{
	if (!Utils::FolderExists(FONT_FOLDER))
	{
		CreateDefaultFontsFolder(true);
	}
}

void FontList::CreateDefaultFontsFolder(bool makeFolder)
{
	if (makeFolder)
	{
		std::filesystem::create_directories(FONT_FOLDER);
	}
}

void FontList::RegisterFontsIntoAtlas(ImFontAtlas* atlas, float defaultFontSize)
{
	if (!atlas) return;

	cachedFontValid = false;
	for (auto& entry : fonts)
	{
		if (!entry.path.empty())
		{
			entry.font = atlas->AddFontFromFileTTF(entry.path.c_str(), defaultFontSize);
			if (!entry.font)
			{
				std::cout << "Failed to load font from path: " << entry.path << std::endl;
			}
		}
	}
}