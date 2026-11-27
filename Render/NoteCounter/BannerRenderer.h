#pragma once

#include <memory>
#include <string>

struct ImDrawList;
struct ImFont;
class GPUImage;
class MIDIApp;

// The opening banner baked into a video render, or shown in the interactive preview when that
// option is enabled. It follows the selected visual: the UMP-style title card (whose colors,
// opacity and scale are configurable), or a recreation of Synthesia's full-width song title band
// when the Synthesia renderer is active. Synthesia's band stays theme-driven and has no color
// settings.
// The Synthesia band follows the app theme: the blue Crystal Blue band in light mode, the dark
// Synthesia Classic band in dark mode. In the preview the UMP card's first line reads "Now
// playing" instead of the render credit.
class BannerRenderer
{
public:
	explicit BannerRenderer(MIDIApp* app) : app(app) {}
	~BannerRenderer();

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

	struct Reveal
	{
		float alpha = 1.0f;
		float open = 1.0f;
		float offsetY = 0.0f;
	};

	Style GetStyle() const;
	std::string GetTitle() const;
	ImFont* GetUMPFont() const;
	Reveal GetReveal(float time, int height) const;
	void EnsureCustomImage();
	void DrawCustomImage(ImDrawList* draw, int width, int height, float bannerTop,
		float bannerHeight, float offsetY, float alpha) const;

	void DrawUMP(ImDrawList* draw, int width, int height, float time, const Reveal& reveal) const;
	void DrawSynthesia(ImDrawList* draw, int width, int height, float time,
		const Reveal& reveal) const;

	MIDIApp* app;
	std::unique_ptr<GPUImage> customImage;
	std::string customImagePath;
};
