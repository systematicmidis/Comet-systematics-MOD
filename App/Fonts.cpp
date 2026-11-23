#include "Fonts.h"
#include "../Inter.hpp"
#include "../FiraCode.hpp"
#include <cstdlib>
#include <filesystem>

ImFont* Fonts::DefaultFont = nullptr;
ImFont* Fonts::MonoFont = nullptr;
ImFont* Fonts::UMPCardFont = nullptr;

void Fonts::LoadFonts()
{
	ImGuiIO& io = ImGui::GetIO();

	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;
	DefaultFont = io.Fonts->AddFontFromMemoryTTF((void*)Inter_18pt_Regular_ttf_data, Inter_18pt_Regular_ttf_size, 18.0f, &cfg);
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