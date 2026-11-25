#include "Localization.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

namespace
{
	using json = nlohmann::json;
	namespace fs = std::filesystem;

	constexpr const char* LOCALE_FOLDER = "./assets/locales";
	constexpr const char* DEFAULT_LANGUAGE = "en-US";
}

void Localization::PreloadLanguages()
{
	loadedLanguages.clear();

	std::error_code ec;
	if (!fs::exists(LOCALE_FOLDER, ec))
	{
		std::cout << "[Localization] Locale folder not found: " << LOCALE_FOLDER << std::endl;
		return;
	}

	for (const auto& entry : fs::directory_iterator(LOCALE_FOLDER, ec))
	{
		if (ec) break;
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".json") continue;

		std::ifstream file(entry.path(), std::ios::in);
		if (!file.is_open()) continue;

		try
		{
			json data;
			file >> data;

			LanguageKey key;
			key.id = entry.path().stem().string();
			key.label = data.value("ID", key.id);
			if (key.label.empty()) key.label = key.id;

			// Do not show the same language twice if a file is copied under a new name.
			const bool duplicate = std::any_of(loadedLanguages.begin(), loadedLanguages.end(),
				[&key](const LanguageKey& existing) { return existing.id == key.id; });
			if (!duplicate)
				loadedLanguages.push_back(std::move(key));
		}
		catch (const json::exception& e)
		{
			std::cout << "[Localization] Failed to inspect " << entry.path().filename().string()
				<< ": " << e.what() << std::endl;
		}
	}

	std::sort(loadedLanguages.begin(), loadedLanguages.end(),
		[](const LanguageKey& a, const LanguageKey& b) { return a.label < b.label; });

	// Load the English fallback once, so a partially translated file still has usable text.
	defaultStrings.clear();
	LoadFile(DEFAULT_LANGUAGE, defaultStrings);
}

bool Localization::LoadFile(const std::string& id,
	std::unordered_map<std::string, std::string>& out) const
{
	const fs::path path = fs::path(LOCALE_FOLDER) / (id + ".json");
	std::ifstream file(path, std::ios::in);
	if (!file.is_open()) return false;

	try
	{
		json data;
		file >> data;

		out.clear();
		for (const auto& [key, value] : data.items())
		{
			if (key == "ID" || !value.is_string()) continue;
			out.emplace(key, value.get<std::string>());
		}
		return true;
	}
	catch (const json::exception& e)
	{
		std::cout << "[Localization] Failed to load " << path.filename().string()
			<< ": " << e.what() << std::endl;
		return false;
	}
}

bool Localization::LoadLanguage(const std::string& language)
{
	if (loadedLanguages.empty())
		PreloadLanguages();

	auto it = std::find_if(loadedLanguages.begin(), loadedLanguages.end(),
		[&language](const LanguageKey& entry) { return entry.id == language; });
	if (it == loadedLanguages.end())
	{
		it = std::find_if(loadedLanguages.begin(), loadedLanguages.end(),
			[](const LanguageKey& entry) { return entry.id == DEFAULT_LANGUAGE; });
		if (it == loadedLanguages.end())
		{
			strings.clear();
			return false;
		}
	}

	std::unordered_map<std::string, std::string> loaded;
	if (!LoadFile(it->id, loaded))
		return false;

	strings = std::move(loaded);
	currentLanguage = static_cast<std::size_t>(std::distance(loadedLanguages.begin(), it));
	return true;
}

std::string Localization::Get(const std::string& key, const std::string& fallback) const
{
	auto it = strings.find(key);
	if (it != strings.end()) return it->second;

	it = defaultStrings.find(key);
	if (it != defaultStrings.end()) return it->second;

	return fallback.empty() ? "[" + key + "]" : fallback;
}

const Localization::LanguageKey* Localization::GetCurrentLanguage() const
{
	if (loadedLanguages.empty()) return nullptr;
	if (currentLanguage >= loadedLanguages.size()) return &loadedLanguages.front();
	return &loadedLanguages[currentLanguage];
}

bool Localization::LanguageExists(const std::string& id) const
{
	return std::any_of(loadedLanguages.begin(), loadedLanguages.end(),
		[&id](const LanguageKey& entry) { return entry.id == id; });
}
