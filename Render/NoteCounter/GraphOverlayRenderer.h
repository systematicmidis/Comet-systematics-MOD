#pragma once

#include "AbstractOverlayRenderer.h"
#include "Config/MIDIPlayerConfig.h"

#include "imgui.h"

#include <deque>
#include <string>
#include <glm/vec2.hpp>

// The Graph overlay: a scrolling line graph of the note counter's NPS and polyphony, anchored to
// the left by default. The numbers down the side use the same font as the note counter, and the
// y-axis rescales itself to whatever is currently on screen so quiet and busy passages both read.
//
// Its rates come from the same measurement the note counter uses (MIDIApp::GetPlain*()), so the two
// overlays always agree. NPS and polyphony can share one axis or be drawn as two stacked graphs.
class GraphOverlayRenderer : public AbstractOverlayRenderer
{
public:
	GraphOverlayRenderer(MIDIApp* app) : AbstractOverlayRenderer(app) {}

	bool IsShown() override;

	void Render(float heightOffset) override;
	void OnResize(int width, int height) override;

	glm::vec2 GetOverlayPosition() const override;
	glm::vec2 GetOverlaySize() const override;

private:
	struct Sample
	{
		double time = 0.0;
		double nps = 0.0;
		double poly = 0.0;
	};

	// Records one point, dropping history that scrolled off the left edge. A seek (time moving
	// backwards) clears what came before so the line doesn't jump across the graph.
	void PushSample(double time, double nps, double poly, double windowSeconds);

	// Draws one graph in the given rectangle: grid, y-axis numbers down its left edge, and whichever
	// series are selected. `axisSlot` picks the smoothed axis to use, so a split NPS/polyphony
	// layout scales each half independently.
	void DrawPlot(ImDrawList* draw, ImVec2 origin, ImFont* font, float fontSize, float textScale,
		ImU32 textColor, ImU32 gridColor, ImU32 npsColor, ImU32 polyColor,
		bool showNps, bool showPoly, float left, float top, float right, float bottom, int axisSlot);

	int width = 0, height = 0;

	// Last panel geometry in screen pixels, kept so the blur behind it lines up with what was drawn.
	float lastPanelX = 0.0f;
	float lastPanelY = 0.0f;
	float lastPanelWidth = 0.0f;
	float lastPanelHeight = 0.0f;

	std::deque<Sample> samples{};

	// The multiplier settings the current history was sampled under. When they change, the history
	// is dropped so old points can't disagree with the counter's current numbers.
	std::string sampleSignature;

	// Per-plot zoom state. The plotted ceiling moves toward the next round value in log10 space, so a
	// change from 2 to 20 and one from 2e9 to 2e10 look the same. Zooming out (the axis grows to fit
	// a new peak) rides a timed glide whose configurable min-max span scales with how tall the graph
	// gets. Zooming in (a peak leaves the window and the axis shrinks back) uses a damped spring that
	// eases in gently then accelerates, with a speed cap so a huge multiplier can't whoosh. Log space
	// keeps it perceptually even across the enormous ranges the multiplier can produce, and doing it
	// in time (not per frame) makes the motion identical at 60 Hz and 120 Hz.
	struct AxisZoom
	{
		double value = 1.0;     // current plotted ceiling
		double velocity = 0.0;  // log10 units per second
		bool initialised = false;
	};

	AxisZoom axisZoom[2];
};
