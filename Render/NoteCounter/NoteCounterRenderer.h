#pragma once

#include "AbstractOverlayRenderer.h"
#include "NoteCounterInfo.h"
#include "NoteCounterStyles.h"
#include "TrackNoteCounter.h"
#include "Config/MIDIPlayerConfig.h"
#include <memory>
#include <array>
#include <glm/glm.hpp>
#include "imgui.h"

class NoteCounterRenderer : public AbstractOverlayRenderer
{
public:
	NoteCounterRenderer(std::shared_ptr<NoteCounterInfo> noteCounterInfo, MIDIApp* app) : AbstractOverlayRenderer(app), noteCounterInfo(noteCounterInfo) {}
	
	bool IsShown() override;

	void Render(float heightOffset) override;
	void OnResize(int width, int height) override;

	void RenderUMP(float scale);
	void RenderMIDITrail(float scale);

	glm::vec2 GetOverlayPosition() const override;
	glm::vec2 GetOverlaySize() const override;
	float GetCounterHeight() const;
private:
	// Keeps the counter on screen when a multiplied note count gets wide enough to spill out of the
	// render target: the content width is clamped to the viewport and the value wraps when it still
	// can't fit. The window itself is anchored to the chosen Alignment edge so it grows inward.
	float ClampCounterWidth(float desiredWidth, float windowPaddingX) const;
	void ApplyCounterAlignment(float heightOffset);

	std::shared_ptr<NoteCounterInfo> noteCounterInfo;

	// How much of each metric came from tracks the multiplier leaves alone. Read from MIDIApp, which
	// measures the split once per frame so the Graph overlay plots the same numbers this shows.
	uint64_t playedPlainNotes = 0;
	uint64_t plainNotesPerSecond = 0;
	uint64_t plainPolyphonyNotes = 0;

	int width = 0, height = 0;
	int counterWidth = DEFAULT_NOTE_COUNTER_WIDTH;
	float lastCounterYOffset = 0.0f;
	float lastCounterWidth = 0.0f;
	float lastCounterHeight = 0.0f;
};