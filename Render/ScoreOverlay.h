#pragma once

#include <memory>

class MIDIApp;
class GPUImage;

// Shared score view. It draws a white four-measure grand staff across the top of the frame,
// above the renderer and its overlays, so every visual mode can show it.
class ScoreOverlay
{
public:
	explicit ScoreOverlay(MIDIApp* app) : app(app) {}

	void Render();

private:
	void LoadTextures();
	void DrawGlyph(GPUImage* image, float centerX, float centerY, float height, uint32_t tint);
	void DrawGlyphAnchored(GPUImage* image, float left, float top, float height, uint32_t tint);

	MIDIApp* app;
	std::unique_ptr<GPUImage> sheetTreble;
	std::unique_ptr<GPUImage> sheetBass;
	std::unique_ptr<GPUImage> sheetSharp;
	std::unique_ptr<GPUImage> sheetFlat;
	std::unique_ptr<GPUImage> sheetNatural;
	std::unique_ptr<GPUImage> noteBlack;
	std::unique_ptr<GPUImage> noteHalf;
	std::unique_ptr<GPUImage> noteWhole;
	std::unique_ptr<GPUImage> flagUp;
	std::unique_ptr<GPUImage> flagDown;
	bool texturesLoaded = false;
};
