#pragma once

#include <string>

struct ImDrawList;
struct ImFont;
class MIDIApp;

// The opening banner baked into a video render, or shown in the interactive preview when that
// option is enabled. It follows the selected visual: the white UMP-style title card, or a
// recreation of Synthesia's full-width song title band when the Synthesia renderer is active.
// The Synthesia band follows the app theme: the blue Crystal Blue band in light mode, the dark
// Synthesia Classic band in dark mode. In the preview the UMP card's first line reads "Now
// playing" instead of the render credit.
class BannerRenderer
{
public:
	explicit BannerRenderer(MIDIApp* app) : app(app) {}

	// Called as a render starts, so every exported video begins with the banner.
	void Reset();

	// Draws the banner if it is enabled and still within its opening window. Renders use the
	// exported frame number; the preview uses the timer's pre-roll.
	void Render();

private:
	enum class Style
	{
		UMP,
		Synthesia
	};

	Style GetStyle() const;
	std::string GetTitle() const;
	ImFont* GetUMPFont() const;

	void DrawUMP(ImDrawList* draw, int width, int height, float time) const;
	void DrawSynthesia(ImDrawList* draw, int width, int height, float time) const;

	MIDIApp* app;
};
