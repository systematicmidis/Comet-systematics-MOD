#include "Fonts.h"
#include "../Inter.hpp"
#include "../FiraCode.hpp"
#include <cstdlib>
#include <filesystem>

namespace
{
	// Locale text needs scripts the embedded Inter font does not cover. The Windows fonts are
	// already installed on every target machine, so merge them instead of adding ~180 MB of
	// bundled CJK fonts to the release. ImGui's dynamic atlas only rasterizes glyphs that are used.
	void MergeSystemFallbackFonts(ImGuiIO& io, float size)
	{
#ifdef _WIN32
		std::filesystem::path fontDir;
		if (const char* windir = std::getenv("WINDIR"))
			fontDir = std::filesystem::path(windir) / "Fonts";
		else
			fontDir = "C:/Windows/Fonts";

		struct FallbackFont
		{
			const char* file;
			ImU32 faceIndex;
		};

		const FallbackFont fallbacks[] = {
			{ "segoeui.ttf", 0 },  // Greek, Cyrillic and broad Latin coverage
			{ "tahoma.ttf", 0 },   // Arabic
			{ "YuGothM.ttc", 0 },  // Japanese
			{ "msyh.ttc", 0 },     // Simplified/traditional Chinese
			{ "malgun.ttf", 0 },   // Korean
			{ "Nirmala.ttc", 0 }   // Devanagari (Hindi)
		};

		for (const FallbackFont& fallback : fallbacks)
		{
			const std::filesystem::path path = fontDir / fallback.file;
			std::error_code ec;
			if (!std::filesystem::exists(path, ec) || ec) continue;

			ImFontConfig cfg;
			cfg.MergeMode = true;
			cfg.FontNo = fallback.faceIndex;
			cfg.PixelSnapH = true;
			cfg.OversampleH = 1;
			cfg.OversampleV = 1;
			io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &cfg);
		}
#endif
	}
}

ImFont* Fonts::DefaultFont = nullptr;
ImFont* Fonts::MonoFont = nullptr;
ImFont* Fonts::UMPCardFont = nullptr;

void Fonts::LoadFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;
	DefaultFont = io.Fonts->AddFontFromMemoryTTF((void*)Inter_18pt_Regular_ttf_data, Inter_18pt_Regular_ttf_size, 18.0f, &cfg);
	MergeSystemFallbackFonts(io, 18.0f);
	io.FontDefault = DefaultFont;
	MonoFont = io.Fonts->AddFontFromMemoryTTF((void*)FiraCode_Regular_ttf_data, FiraCode_Regular_ttf_size, 18.0f, &cfg);

#ifdef _WIN32
	// UMP's card is set in Comic Sans. Use the copy that ships with Windows rather than bundling a
	// font, and keep the default app font as a fallback on systems without it.
	std::filesystem::path comicPath;
	if (const char* windir = std::getenv("WINDIR"))
		comicPath = std::filesystem::path(windir) / "Fonts" / "comic.ttf";
	else
		comicPath = "C:/Windows/Fonts/comic.ttf";

	if (std::filesystem::exists(comicPath))
		UMPCardFont = io.Fonts->AddFontFromFileTTF(comicPath.string().c_str(), 18.0f);
#endif

	if (UMPCardFont == nullptr)
		UMPCardFont = DefaultFont;
}