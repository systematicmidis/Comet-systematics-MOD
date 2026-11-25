#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef TRANSLATE
#define TRANSLATE(key) localization->Get(key)
#endif

class Localization
{
public:
	// Scans ./assets/locales for language files. Safe to call more than once.
	void PreloadLanguages();

	// Loads the requested language, falling back to en-US when it is missing or invalid.
	bool LoadLanguage(const std::string& language);

	// Returns the translated string. Missing keys fall back to en-US, then to the supplied
	// fallback, then to "[key]" so a missing translation is obvious but never empty.
	std::string Get(const std::string& key, const std::string& fallback = "") const;

	struct LanguageKey
	{
		std::string id;
		std::string label;
	};

	const std::vector<LanguageKey>& GetLanguageList() const { return loadedLanguages; }
	const LanguageKey* GetCurrentLanguage() const;
	bool LanguageExists(const std::string& id) const;

private:
	bool LoadFile(const std::string& id, std::unordered_map<std::string, std::string>& out) const;

	std::unordered_map<std::string, std::string> strings{};
	std::unordered_map<std::string, std::string> defaultStrings{};
	std::vector<LanguageKey> loadedLanguages{};
	std::size_t currentLanguage = 0;
};
