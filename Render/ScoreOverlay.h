#pragma once

#include <memory>

class MIDIApp;
class GPUImage;

// Shared score view. It draws a full white four-measure page with a grand staff, above the
// renderer and its overlays, so every visual mode can show it like Plus2's Score View.
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
	std::unique_ptr<GPUImage> sheetBrace;
	std::unique_ptr<GPUImage> noteBlack;
	std::unique_ptr<GPUImage> noteHalf;
	std::unique_ptr<GPUImage> noteWhole;
	std::unique_ptr<GPUImage> sheetRestWhole;
	std::unique_ptr<GPUImage> flagUp;
	std::unique_ptr<GPUImage> flagDown;
	std::unique_ptr<GPUImage> flagUp16;
	std::unique_ptr<GPUImage> flagDown16;
	std::unique_ptr<GPUImage> flagUp32;
	std::unique_ptr<GPUImage> flagDown32;
	std::unique_ptr<GPUImage> flagUp64;
	std::unique_ptr<GPUImage> flagDown64;
	std::unique_ptr<GPUImage> sheetTime[10];
	bool texturesLoaded = false;
};
