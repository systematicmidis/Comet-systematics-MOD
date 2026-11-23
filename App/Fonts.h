#pragma once

#include <imgui.h>

namespace Fonts
{
	extern ImFont* DefaultFont;
	extern ImFont* MonoFont;
	// The typeface UMP uses on its opening card. It is loaded from the system when available and
	// falls back to the default app font.
	extern ImFont* UMPCardFont;
	
	void LoadFonts();
}