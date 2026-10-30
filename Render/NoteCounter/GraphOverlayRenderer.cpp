#include "GraphOverlayRenderer.h"

#include "App/Fonts.h"
#include "App/FontList.h"
#include "App/MIDIApp.h"
#include "NoteCounterInfo.h"
#include "NoteMultiplier.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// One sample per this many seconds. The app can run at 120 Hz; 60 Hz is plenty for a graph this
	// wide and keeps the per-frame line work bounded.
	constexpr double SAMPLE_INTERVAL = 1.0 / 60.0;

	// Ceiling for multiplied values: a googol times a few thousand must saturate, not overflow.
	constexpr double MAX_PLOT_VALUE = 1.0e300;

	// The zoom spring, in log10 space, used when the axis is *shrinking* (a peak has scrolled off the
	// left edge and the scale can come back down). The stiffness/damping pair is roughly critically
	// damped (damping ~ 2 * sqrt(stiffness)), so the scale settles without overshooting, and the
	// speed cap limits how fast the axis can move no matter how big the jump - the zoom starts
	// gently and speeds up, but never whooshes.
	constexpr double ZOOM_IN_STIFFNESS = 30.0;    // log10 units / s^2 per decade of error
	constexpr double ZOOM_IN_DAMPING = 11.0;      // log10 units / s
	constexpr double ZOOM_IN_MAX_SPEED = 2.0;     // decades per second

	// Zooming *out* (the axis has to grow to fit a new peak) is a timed glide rather than a spring:
	// the axis takes a deliberate span of seconds to open up, and a taller graph takes longer. The
	// bounds of that span are configurable (graph.zoomOutMinSeconds/zoomOutMaxSeconds); the duration
	// is measured from the peak, so it stays bounded no matter how many decades the multiplier has
	// stacked up.
	//
	// How many e-folds of the gap to fall before calling it settled. exp(-3) is ~95%, so the visible
	// motion finishes close to the nominal number of seconds.
	constexpr double ZOOM_OUT_DECAYS = 3.0;

	// How long a zoom-out from this peak should take, in seconds. Height is read in decades (digits),
	// so a graph that reaches a few tens is quick while one that reaches hundreds of thousands gets
	// the full glide. The half-second-per-decade slope is then clamped into the configured range.
	double ZoomOutSeconds(double peak, double minSeconds, double maxSeconds)
	{
		const double decades = std::log10(std::max(peak, 1.0));
		const double seconds = 1.0 + (decades - 1.0) * 0.5;
		return std::clamp(seconds, minSeconds, maxSeconds);
	}

	// A round step (1, 2 or 5 times a power of ten) for the y-axis.
	double NiceStep(double raw)
	{
		if (raw <= 0.0)
			return 1.0;

		const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
		const double normalised = raw / magnitude;
		const double nice = normalised <= 1.0 ? 1.0 : normalised <= 2.0 ? 2.0 : normalised <= 5.0 ? 5.0 : 10.0;
		return nice * magnitude;
	}

	std::string FormatAxisValue(double value)
	{
		if (value < 1.0e15)
			return Utils::FormatWithCommas((long long)std::llround(value));

		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.3g", value);
		return buf;
	}
}

bool GraphOverlayRenderer::IsShown()
{
	return app->GetConfig()->graph.enabled;
}

void GraphOverlayRenderer::PushSample(double time, double nps, double poly, double windowSeconds)
{
	// A seek moves the playhead backwards; the old history would draw a line jumping across the
	// whole graph, so drop it and start again from here.
	if (!samples.empty() && time < samples.back().time)
		samples.clear();

	if (samples.empty() || (time - samples.back().time) >= SAMPLE_INTERVAL)
		samples.push_back({ time, nps, poly });

	// Keep a little past the window so the leftmost segment still reaches the edge of the plot.
	while (!samples.empty() && (time - samples.front().time) > windowSeconds + SAMPLE_INTERVAL)
		samples.pop_front();
}

void GraphOverlayRenderer::Render(float heightOffset)
{
	MIDIPlayerConfig* config = app->GetConfig();
	const MIDIPlayerConfig::ConfigGraph& graph = config->graph;
	NoteCounterInfo* counter = app->GetNoteCounterInfo();

	const double now = counter->timeSeconds.value;
	const double windowSeconds = std::max(1.0, (double)graph.windowSeconds);
	const bool graphMultiply = config->multiplier.multiplyGraph;

	// The multiplier settings (and the layout) the history was sampled under. If any changed, drop
	// the history: old points carry the old multiplier and would disagree with the counter.
	{
		const MIDIPlayerConfig::ConfigMultiplier& multiplier = config->multiplier;
		std::string signature = multiplier.value;
		signature += multiplier.enabled ? "|on" : "|off";
		signature += multiplier.multiplyGraph ? "|g1" : "|g0";
		signature += graph.separate ? "|s1" : "|s0";
		signature += "|" + std::to_string(multiplier.melodyNotes);
		signature += "|" + std::to_string(multiplier.npsPolyBoostThreshold);
		for (uint16_t track : multiplier.unmultipliedTracks)
			signature += "," + std::to_string(track);
		signature += "|seq" + std::to_string((unsigned long long)(uintptr_t)app->GetRendererSequence());
		// The counter/graph source can differ from the rendered MIDI (a .CJCMCG chart); a change
		// there invalidates the history just like loading a different MIDI does.
		signature += "|cs" + std::to_string((unsigned long long)(uintptr_t)app->GetCounterChart());

		if (signature != sampleSignature)
		{
			sampleSignature = signature;
			samples.clear();
			axisZoom[0] = AxisZoom{};
			axisZoom[1] = AxisZoom{};
		}
	}

	// The rates and the per-track plain split come from the same place the counter reads them, so
	// the graph plots exactly the numbers the counter prints - from the very first note.
	const double npsRate = std::min(
		NoteMultiplier::DisplayedRate(config, counter->notesPerSecond.value, counter->notesPassed.value,
			app->GetPlainNotesPerSecond(), graphMultiply, app->GetPlainPlayedNotes()),
		MAX_PLOT_VALUE);
	const double polyRate = std::min(
		NoteMultiplier::DisplayedRate(config, counter->polyphony.value, counter->notesPassed.value,
			app->GetPlainPolyphony(), graphMultiply, app->GetPlainPlayedNotes()),
		MAX_PLOT_VALUE);

	PushSample(now, npsRate, polyRate, windowSeconds);

	// --- geometry ---
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float panelWidth = std::max(80.0f, display.x * graph.widthFraction);
	const float panelHeight = std::max(60.0f, display.y * graph.heightFraction);

	float panelX = 0.0f;
	if (graph.alignment == NoteCounterAlignment::TopCenter)
		panelX = (display.x - panelWidth) * 0.5f;
	else if (graph.alignment == NoteCounterAlignment::TopRight)
		panelX = display.x - panelWidth;

	const float panelY = heightOffset;

	// The numbers use the same font as the note counter, as requested.
	ImFont* fontToUse = nullptr;
	for (const auto& f : app->GetFontList()->GetFonts())
	{
		if (f.path == config->overlayInfo.selectedFontPath)
		{
			fontToUse = f.font;
			break;
		}
	}

	const float textScale = std::max(0.05f, config->overlayInfo.scale);

	ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, config->overlayInfo.backgroundCol);

	if (fontToUse) ImGui::PushFont(fontToUse);
	else ImGui::PushFont(Fonts::MonoFont);

	if (ImGui::Begin("graphOverlay", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus))
	{
		lastPanelX = panelX;
		lastPanelY = panelY;
		lastPanelWidth = panelWidth;
		lastPanelHeight = panelHeight;

		ImDrawList* draw = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetWindowPos();

		ImFont* font = ImGui::GetFont();
		const float fontSize = std::max(6.0f, ImGui::GetFontSize() * textScale);
		const ImU32 textColor = ImGui::GetColorU32(config->overlayInfo.textCol);
		const ImU32 gridColor = ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f, 0.85f));
		const ImU32 npsColor = ImGui::ColorConvertFloat4ToU32(graph.npsColor);
		const ImU32 polyColor = ImGui::ColorConvertFloat4ToU32(graph.polyColor);

		const float panelRight = origin.x + panelWidth;
		const float panelBottom = origin.y + panelHeight;

		if (graph.separate)
		{
			// NPS on top, polyphony below, each scaling to its own data.
			const float midY = origin.y + panelHeight * 0.5f;
			DrawPlot(draw, origin, font, fontSize, textScale, textColor, gridColor, npsColor, polyColor,
				true, false, origin.x, origin.y, panelRight, midY, 0);
			DrawPlot(draw, origin, font, fontSize, textScale, textColor, gridColor, npsColor, polyColor,
				false, true, origin.x, midY, panelRight, panelBottom, 1);
		}
		else
		{
			DrawPlot(draw, origin, font, fontSize, textScale, textColor, gridColor, npsColor, polyColor,
				true, true, origin.x, origin.y, panelRight, panelBottom, 0);
		}
	}
	ImGui::End();

	ImGui::PopFont();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

void GraphOverlayRenderer::DrawPlot(ImDrawList* draw, ImVec2 origin, ImFont* font, float fontSize,
	float textScale, ImU32 textColor, ImU32 gridColor, ImU32 npsColor, ImU32 polyColor,
	bool showNps, bool showPoly, float left, float top, float right, float bottom, int axisSlot)
{
	MIDIPlayerConfig* config = app->GetConfig();
	const double now = app->GetNoteCounterInfo()->timeSeconds.value;
	const double windowSeconds = std::max(1.0, (double)config->graph.windowSeconds);

	// --- y-axis (from whichever series this plot shows) ---
	double peak = 0.0;
	for (const Sample& s : samples)
	{
		if (showNps) peak = std::max(peak, s.nps);
		if (showPoly) peak = std::max(peak, s.poly);
	}

	peak = std::max(peak, 1.0);

	// Frame time drives the easing, so the motion looks identical at any refresh rate. Clamped so a
	// hitch (or the very first frame) can't make the axis leap.
	const double dt = std::clamp((double)ImGui::GetIO().DeltaTime, 1.0 / 1000.0, 0.1);

	const int intervals = 5;

	// The round ceiling this plot is heading for, based on the instantaneous peak.
	const double stepFor = NiceStep(peak / intervals);
	const double target = std::max(stepFor, std::ceil(peak / stepFor) * stepFor);

	// Move the plotted ceiling toward the round target in log10 space. Zooming *out* (the axis has to
	// grow to fit a new peak) rides a timed exponential over the configured min-max span, scaled by
	// how tall the graph got, so a big spike doesn't whiplash the scale. Zooming *in* (the axis shrinks
	// back as a peak scrolls off) starts at rest and gently accelerates, capped so it can't whoosh.
	// Working in log space keeps the pacing even whether the scale is 2 or 2e20.
	AxisZoom& zoom = axisZoom[axisSlot];
	const double logTarget = std::log10(std::max(target, 1.0));

	if (!zoom.initialised)
	{
		zoom.value = std::max(target, 1.0);
		zoom.velocity = 0.0;
		zoom.initialised = true;
	}
	else
	{
		const double logValue = std::log10(std::max(zoom.value, 1.0));

		if (logTarget > logValue)
		{
			// Zoom out. The time constant is fixed by the peak, so the gap closes over roughly
			// ZoomOutSeconds(); because it is exponential it starts at its fastest and eases into
			// place, and the residual velocity is dropped so a later zoom-in starts from rest.
			const double zoomOutMin = std::max(0.05, (double)config->graph.zoomOutMinSeconds);
			const double zoomOutMax = std::max(zoomOutMin, (double)config->graph.zoomOutMaxSeconds);
			const double tau = ZoomOutSeconds(peak, zoomOutMin, zoomOutMax) / ZOOM_OUT_DECAYS;
			const double blend = 1.0 - std::exp(-dt / tau);
			zoom.value = std::pow(10.0, std::clamp(logValue + (logTarget - logValue) * blend, 0.0, 300.0));
			zoom.velocity = 0.0;
		}
		else
		{
			const double accel = ZOOM_IN_STIFFNESS * (logTarget - logValue) - ZOOM_IN_DAMPING * zoom.velocity;
			zoom.velocity = std::clamp(zoom.velocity + accel * dt, -ZOOM_IN_MAX_SPEED, ZOOM_IN_MAX_SPEED);
			zoom.value = std::pow(10.0, std::clamp(logValue + zoom.velocity * dt, 0.0, 300.0));
		}
	}

	// The drawn ceiling follows the eased value, and the labels are picked to fill it.
	const double yTop = std::max(zoom.value, 1.0);
	const double step = NiceStep(yTop / intervals);

	// The widest label decides the left gutter the numbers live in.
	float gutter = 0.0f;
	for (int i = 0; i <= intervals; ++i)
		gutter = std::max(gutter, ImGui::CalcTextSize(FormatAxisValue(step * i).c_str()).x * textScale);

	const float pad = 6.0f * textScale;
	const float plotLeft = left + gutter + pad * 2.0f;
	const float plotRight = right - pad;
	const float plotTop = top + pad;
	const float plotBottom = bottom - pad;
	const float plotWidth = std::max(1.0f, plotRight - plotLeft);
	const float plotHeight = std::max(1.0f, plotBottom - plotTop);

	auto yFor = [&](double value)
	{
		const double t = std::clamp(value / yTop, 0.0, 1.0);
		return (float)(plotBottom - t * plotHeight);
	};

	auto xFor = [&](double time)
	{
		const double t = std::clamp((time - (now - windowSeconds)) / windowSeconds, 0.0, 1.0);
		return (float)(plotLeft + t * plotWidth);
	};

	// --- grid and numbers ---
	for (int i = 0; i <= intervals; ++i)
	{
		const double value = step * i;
		if (value > yTop * 1.0001)
			break;

		const float y = yFor(value);
		draw->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y), gridColor, 1.0f);

		const std::string label = FormatAxisValue(value);
		const float labelWidth = ImGui::CalcTextSize(label.c_str()).x * textScale;
		draw->AddText(font, fontSize, ImVec2(left + gutter - labelWidth + pad, y - fontSize * 0.5f),
			textColor, label.c_str());
	}

	// --- series ---
	if (samples.size() < 2)
		return;

	draw->PushClipRect(ImVec2(plotLeft, plotTop), ImVec2(plotRight, plotBottom), true);

	const float thickness = std::max(1.0f, 2.0f * textScale);

	std::vector<ImVec2> points;
	points.reserve(samples.size());

	auto drawSeries = [&](bool polySeries, ImU32 color)
	{
		points.clear();
		for (const Sample& s : samples)
			points.emplace_back(xFor(s.time), yFor(polySeries ? s.poly : s.nps));

		draw->AddPolyline(points.data(), (int)points.size(), color, thickness, 0);
	};

	if (showNps) drawSeries(false, npsColor);
	if (showPoly) drawSeries(true, polyColor);

	draw->PopClipRect();
}

void GraphOverlayRenderer::OnResize(int width, int height)
{
	this->width = width;
	this->height = height;
}

glm::vec2 GraphOverlayRenderer::GetOverlayPosition() const
{
	if (width <= 0 || height <= 0)
		return glm::vec2(0.0f);

	// Normalised with a bottom-left origin, matching how the blur-behind quad is drawn.
	const float x = lastPanelX / (float)width;
	const float y = 1.0f - (lastPanelY + lastPanelHeight) / (float)height;
	return glm::vec2(x, y);
}

glm::vec2 GraphOverlayRenderer::GetOverlaySize() const
{
	if (width <= 0 || height <= 0)
		return glm::vec2(0.0f);

	return glm::vec2(lastPanelWidth / (float)width, lastPanelHeight / (float)height);
}
