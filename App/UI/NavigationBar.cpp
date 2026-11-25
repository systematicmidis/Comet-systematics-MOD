#include "NavigationBar.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Utils.h"
#include "App/MIDIApp.h"
#include "App/MainWindow.h"
#include "MIDI/Timer/MIDITimer.h"
#include "App/UI/Widgets/ThinSlider.h"
#include "App/UI/Themes/Themes.h"

#define NAVIGATION_ROWS 2
#define MIN_MIDI_TIME_SECS -3.0f

#define UI_ATLAS_CELL_X 4
#define UI_ATLAS_CELL_Y 2

#define ATLAS_UV(atlas, x, y) \
    ImVec2((float)(x) / UI_ATLAS_CELL_X, (float)(y) / UI_ATLAS_CELL_Y), \
    ImVec2((float)(x + 1) / UI_ATLAS_CELL_X, (float)(y + 1) / UI_ATLAS_CELL_Y)

namespace
{
	// Synthesia's play bar keeps its own dark chrome no matter which Comet colour palette is
	// active, so the bar still matches the reference while the rest of the UI follows the theme.
	const ImVec4 SYNTH_BAR_BG = ImVec4(0.169f, 0.176f, 0.192f, 1.0f); // #2b2d31
	const ImVec4 SYNTH_BAR_BORDER = ImVec4(0.247f, 0.259f, 0.278f, 1.0f); // #3f4247
	const ImVec4 SYNTH_BUTTON_BG = ImVec4(0.216f, 0.224f, 0.243f, 1.0f); // #37393e
	const ImVec4 SYNTH_BUTTON_HOVER = ImVec4(0.267f, 0.278f, 0.302f, 1.0f);
	const ImVec4 SYNTH_BUTTON_ACTIVE = ImVec4(0.318f, 0.329f, 0.357f, 1.0f);
	const ImVec4 SYNTH_TEXT = ImVec4(0.910f, 0.918f, 0.933f, 1.0f);
	const ImVec4 SYNTH_TEXT_DIM = ImVec4(0.733f, 0.745f, 0.769f, 1.0f);
	const ImVec4 SYNTH_SEEK_BG = ImVec4(0.294f, 0.306f, 0.333f, 1.0f);
	const ImVec4 SYNTH_SEEK_PROGRESS = ImVec4(0.616f, 0.631f, 0.655f, 1.0f);
	const ImVec4 SYNTH_ICON_HOVER = ImVec4(1.0f, 1.0f, 1.0f, 0.09f);

	constexpr float SYNTH_BAR_HEIGHT = 48.0f;

	enum class SynthIcon
	{
		Settings,
		Search,
		Counter,
		Fullscreen,
		Menu
	};

	void DrawGearIcon(ImDrawList* dl, const ImVec2& c, ImU32 col)
	{
		dl->AddCircle(c, 5.0f, col, 0, 1.6f);
		dl->AddCircle(c, 2.0f, col, 0, 1.4f);
		for (int i = 0; i < 8; i++)
		{
			const float a = i * 3.14159265f / 4.0f;
			const float dx = std::cos(a), dy = std::sin(a);
			dl->AddLine(
				ImVec2(c.x + dx * 6.0f, c.y + dy * 6.0f),
				ImVec2(c.x + dx * 8.5f, c.y + dy * 8.5f),
				col, 1.8f);
		}
	}

	void DrawSearchIcon(ImDrawList* dl, const ImVec2& c, ImU32 col)
	{
		dl->AddCircle(ImVec2(c.x - 1.5f, c.y - 1.5f), 5.0f, col, 0, 1.6f);
		dl->AddLine(ImVec2(c.x + 2.0f, c.y + 2.0f), ImVec2(c.x + 7.0f, c.y + 7.0f), col, 2.0f);
	}

	void DrawCounterIcon(ImDrawList* dl, const ImVec2& c, ImU32 col)
	{
		const float heights[3] = { 6.0f, 11.0f, 8.0f };
		for (int i = 0; i < 3; i++)
		{
			const float x = c.x - 7.0f + i * 5.5f;
			dl->AddRectFilled(ImVec2(x, c.y + 6.0f - heights[i]), ImVec2(x + 3.0f, c.y + 6.0f), col, 1.0f);
		}
	}

	void DrawFullscreenIcon(ImDrawList* dl, const ImVec2& c, ImU32 col)
	{
		const float s = 7.0f, l = 4.0f;
		dl->AddLine(ImVec2(c.x - s, c.y - s + l), ImVec2(c.x - s, c.y - s), col, 1.6f);
		dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s + l, c.y - s), col, 1.6f);
		dl->AddLine(ImVec2(c.x + s - l, c.y - s), ImVec2(c.x + s, c.y - s), col, 1.6f);
		dl->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x + s, c.y - s + l), col, 1.6f);
		dl->AddLine(ImVec2(c.x + s, c.y + s - l), ImVec2(c.x + s, c.y + s), col, 1.6f);
		dl->AddLine(ImVec2(c.x + s, c.y + s), ImVec2(c.x + s - l, c.y + s), col, 1.6f);
		dl->AddLine(ImVec2(c.x - s + l, c.y + s), ImVec2(c.x - s, c.y + s), col, 1.6f);
		dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x - s, c.y + s - l), col, 1.6f);
	}

	void DrawMenuIcon(ImDrawList* dl, const ImVec2& c, ImU32 col)
	{
		for (int i = -1; i <= 1; i++)
			dl->AddLine(ImVec2(c.x - 7.0f, c.y + i * 4.0f), ImVec2(c.x + 7.0f, c.y + i * 4.0f), col, 1.6f);
	}

	bool SynthIconButton(SynthIcon icon, const char* id, const char* tooltip)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton(id, ImVec2(26.0f, 26.0f));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hovered)
			dl->AddRectFilled(p, ImVec2(p.x + 26.0f, p.y + 26.0f), ImGui::GetColorU32(SYNTH_ICON_HOVER), 3.0f);

		const ImVec2 c(p.x + 13.0f, p.y + 13.0f);
		const ImU32 col = ImGui::GetColorU32(SYNTH_TEXT);
		switch (icon)
		{
		case SynthIcon::Settings: DrawGearIcon(dl, c, col); break;
		case SynthIcon::Search: DrawSearchIcon(dl, c, col); break;
		case SynthIcon::Counter: DrawCounterIcon(dl, c, col); break;
		case SynthIcon::Fullscreen: DrawFullscreenIcon(dl, c, col); break;
		case SynthIcon::Menu: DrawMenuIcon(dl, c, col); break;
		}

		if (hovered) ImGui::SetTooltip("%s", tooltip);
		return clicked;
	}

	enum class SynthTransport
	{
		Play,
		Pause,
		Back,
		Forward
	};

	// Synthesia's transport uses a play triangle and single skip icons with an end bar, not the
	// atlas's rewind/fast-forward chevrons.
	bool SynthTransportButton(SynthTransport type, const char* id, const char* tooltip)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::InvisibleButton(id, ImVec2(32.0f, 30.0f));
		const bool hovered = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		if (hovered)
			dl->AddRectFilled(p, ImVec2(p.x + 32.0f, p.y + 30.0f), ImGui::GetColorU32(SYNTH_ICON_HOVER), 3.0f);

		const ImU32 col = ImGui::GetColorU32(SYNTH_TEXT);
		const float cx = p.x + 16.0f, cy = p.y + 15.0f;
		switch (type)
		{
		case SynthTransport::Play:
			dl->AddTriangleFilled(ImVec2(cx - 5.0f, cy - 8.0f), ImVec2(cx - 5.0f, cy + 8.0f), ImVec2(cx + 8.0f, cy), col);
			break;
		case SynthTransport::Pause:
			dl->AddRectFilled(ImVec2(cx - 6.0f, cy - 8.0f), ImVec2(cx - 2.0f, cy + 8.0f), col, 1.0f);
			dl->AddRectFilled(ImVec2(cx + 2.0f, cy - 8.0f), ImVec2(cx + 6.0f, cy + 8.0f), col, 1.0f);
			break;
		case SynthTransport::Back:
			dl->AddRectFilled(ImVec2(cx - 8.0f, cy - 8.0f), ImVec2(cx - 5.0f, cy + 8.0f), col, 1.0f);
			dl->AddTriangleFilled(ImVec2(cx + 7.0f, cy - 8.0f), ImVec2(cx + 7.0f, cy + 8.0f), ImVec2(cx - 5.0f, cy), col);
			break;
		case SynthTransport::Forward:
			dl->AddTriangleFilled(ImVec2(cx - 7.0f, cy - 8.0f), ImVec2(cx - 7.0f, cy + 8.0f), ImVec2(cx + 5.0f, cy), col);
			dl->AddRectFilled(ImVec2(cx + 5.0f, cy - 8.0f), ImVec2(cx + 8.0f, cy + 8.0f), col, 1.0f);
			break;
		}

		if (hovered) ImGui::SetTooltip("%s", tooltip);
		return clicked;
	}
}

// TODO: Implement reset when unloading midi

NavigationBar::NavigationBar(MIDIApp* app, RenderView* renderView)
{
	this->app = app;
	this->renderView = renderView;
}

void NavigationBar::TryLoadUITextures()
{
	if (texturesLoaded) return;

#define LOAD_TEXTURE(target, path) \
	target = std::make_unique<GPUImage>(Utils::TryGetStream(path))

	LOAD_TEXTURE(uiTexture, "./assets/textures/ui.png");

#undef LOAD_TEXTURE

	texturesLoaded = true;
}

void NavigationBar::VerticalSeparator(float height, bool altColor)
{
	if (height <= 0.0f) height = ImGui::GetFrameHeight();
	ImVec2 pos = ImGui::GetCursorScreenPos();

	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(pos.x, pos.y),
		ImVec2(pos.x, pos.y + height),
		ImGui::GetColorU32(altColor ? ImGuiCol_Text : ImGuiCol_Separator, altColor ? 0.75f : 1.0f)
	);

	ImGui::Dummy(ImVec2(1.0f, height));
}

void NavigationBar::Draw()
{
	if (!app->IsGLReady()) return;
	TryLoadUITextures();

	// The Synthesia visual theme replaces the floating transport strip with Synthesia's play bar:
	// an opaque strip with Back/Help on the left, centred transport and zoom controls and a row of
	// tool icons on the right. Comet keeps its original two-row strip.
	VisualThemeList* visualThemes = app->GetVisualThemeList();
	VisualTheme* visualTheme = visualThemes != nullptr ? visualThemes->GetCurrentTheme() : nullptr;
	const bool synthesiaToolbar = visualTheme != nullptr && visualTheme->style.framedToolbar;

	if (synthesiaToolbar) DrawSynthesiaToolbar();
	else DrawCometToolbar();

	Update();
}

void NavigationBar::DrawCometToolbar()
{
	ImGuiViewport* viewport = ImGui::GetMainViewport();

	const float toolbarPaddingY = 2.0f;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, toolbarPaddingY));
	ImVec2 pos = viewport->WorkPos;
	ImGui::SetNextWindowPos(pos);
	lastPosition = glm::vec2(pos.x, pos.y);

	float frameHeight = ImGui::GetFrameHeight();
	float framePadding = ImGui::GetStyle().FramePadding.y;

	float spacing = ImGui::GetStyle().ItemSpacing.y;
	float toolbarHeight = frameHeight * NAVIGATION_ROWS + spacing * (NAVIGATION_ROWS - 1) + toolbarPaddingY * 2.0f;

	ImVec2 size = ImVec2(viewport->WorkSize.x, toolbarHeight);
	ImGui::SetNextWindowSize(size);
	lastResolution = glm::vec2(size.x, size.y);

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

	auto& style = ImGui::GetStyle();
	auto& c = style.Colors;
	ImVec4 textCol = c[ImGuiCol_Text];
	ImVec4 bgCol = c[ImGuiCol_WindowBg];
	const ImVec4 iconButtonBg = ImVec4(0, 0, 0, 0);

	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(bgCol.x, bgCol.y, bgCol.z, 0.8f));

	if (ImGui::Begin("MainDockWindow", nullptr, flags))
	{
		float imageSize = frameHeight - framePadding * 2.0f;
		MIDITimer* timer = app->GetTimer().get();
		// top bar
		{
			{
				if (ImGui::ImageButton("##playBtn",
					(ImTextureID)(intptr_t)uiTexture->GetRawTexture(),
					ImVec2(imageSize, imageSize),
					ATLAS_UV(uiTexture, 0, 0),
					iconButtonBg,
					textCol))
				{
					timer->Resume();
				}
				ImGui::SameLine();
				if (ImGui::ImageButton("##pauseBtn",
					(ImTextureID)(intptr_t)uiTexture->GetRawTexture(),
					ImVec2(imageSize, imageSize),
					ATLAS_UV(uiTexture, 1, 0),
					iconButtonBg,
					textCol))
				{
					timer->Pause();
				}
				ImGui::SameLine();
				if (ImGui::ImageButton("##stopBtn",
					(ImTextureID)(intptr_t)uiTexture->GetRawTexture(),
					ImVec2(imageSize, imageSize),
					ATLAS_UV(uiTexture, 2, 0),
					iconButtonBg,
					textCol))
				{
					timer->Stop();
				}
			}

			ImGui::SameLine();
			VerticalSeparator(0.0f, true);
			ImGui::SameLine();

			{
				MIDIPlayerConfig* config = app->GetConfig();

				if (ImGui::ImageButton("##backBtn",
					(ImTextureID)(intptr_t)uiTexture->GetRawTexture(),
					ImVec2(imageSize, imageSize),
					ATLAS_UV(uiTexture, 0, 1),
					iconButtonBg,
					textCol))
				{
					float jump = config->navigation.seekBackwardSeconds;
					float targetTime = timer->Elapsed() - jump;
					if (targetTime < MIN_MIDI_TIME_SECS) targetTime = MIN_MIDI_TIME_SECS;

					timer->NavigateTo(targetTime);
				}

				ImGui::SameLine();

				if (ImGui::ImageButton("##forwardBtn",
					(ImTextureID)(intptr_t)uiTexture->GetRawTexture(),
					ImVec2(imageSize, imageSize),
					ATLAS_UV(uiTexture, 3, 0),
					iconButtonBg,
					textCol))
				{
					float jump = config->navigation.seekForwardSeconds;
					float targetTime = timer->Elapsed() + jump;
					if (targetTime > midiLength) targetTime = midiLength;

					timer->NavigateTo(targetTime);
				}
			}

			ImGui::SameLine();
			VerticalSeparator(0.0f, true);
			ImGui::SameLine();

			ImGui::SetNextItemWidth(170);
			int viewTicks = static_cast<int>(renderView->viewTicks);

			if (ThinSlider::Draw<int>("##navNoteSize", &viewTicks, 48, 7680, "", ImGuiSliderFlags_Logarithmic))
			{
				renderView->viewTicks = std::clamp(viewTicks, 48, 7680);
			}

			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Note size (in ticks)");
			}
		}

		// time bar
		{
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			float currTimeSecs = timer->Elapsed();

			if (ThinSlider::Draw<float>("##navMIDITime", &currTimeSecs, MIN_MIDI_TIME_SECS, midiLength, "", ImGuiSliderFlags_NoInput))
			{
				if (currTimeSecs != timer->Elapsed())
				{
					timer->NavigateTo(currTimeSecs);
				}
			}
		}
	}
	ImGui::End();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

void NavigationBar::DrawSynthesiaToolbar()
{
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	MainWindow* mainWindow = app->GetMainWindow();
	MIDITimer* timer = app->GetTimer().get();
	MIDIPlayerConfig* config = app->GetConfig();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

	const ImVec2 pos = viewport->WorkPos;
	const ImVec2 size(viewport->WorkSize.x, SYNTH_BAR_HEIGHT);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	lastPosition = glm::vec2(pos.x, pos.y);
	lastResolution = glm::vec2(size.x, size.y);

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoBringToFrontOnFocus;

	ImGui::PushStyleColor(ImGuiCol_WindowBg, SYNTH_BAR_BG);
	ImGui::PushStyleColor(ImGuiCol_Text, SYNTH_TEXT);
	ImGui::PushStyleColor(ImGuiCol_Button, SYNTH_BUTTON_BG);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, SYNTH_BUTTON_HOVER);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, SYNTH_BUTTON_ACTIVE);
	ImGui::PushStyleColor(ImGuiCol_Border, SYNTH_BAR_BORDER);

	if (ImGui::Begin("##synthesia_bar", nullptr, flags))
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImU32 textCol = ImGui::GetColorU32(SYNTH_TEXT);
		const float barW = ImGui::GetWindowWidth();
		const float barH = ImGui::GetWindowHeight();

		// bottom edge of the bar
		dl->AddLine(ImVec2(pos.x, pos.y + barH - 0.5f), ImVec2(pos.x + barW, pos.y + barH - 0.5f),
			ImGui::GetColorU32(SYNTH_BAR_BORDER), 1.0f);

		// --- left: Back / Help ---------------------------------------------------------------
		ImGui::SetCursorPos(ImVec2(8.0f, 10.0f));
		if (ImGui::Button("Back", ImVec2(58.0f, 28.0f)))
		{
			// Comet has no song list screen; Back returns to the empty player like Synthesia's
			// play screen returns to the song list.
			timer->Stop();
			app->UnloadMIDI();
		}
		ImGui::SameLine(0.0f, 6.0f);
		if (ImGui::Button("Help", ImVec2(58.0f, 28.0f)))
			ImGui::OpenPopup("##synth_help");

		if (ImGui::BeginPopup("##synth_help"))
		{
			if (ImGui::MenuItem("Buy ponluxime a coffee"))
				Utils::OpenURL("https://ko-fi.com/ponluxime");
			if (ImGui::MenuItem("Check for Updates"))
				mainWindow->OpenUpdateDialog();
			ImGui::Separator();
			if (ImGui::MenuItem("Diagnose MIDI..."))
				mainWindow->OpenDiagnosisDialog();
			ImGui::EndPopup();
		}

		// --- right: tool icons ---------------------------------------------------------------
		constexpr float ICON_W = 26.0f;
		constexpr float ICON_GAP = 6.0f;
		constexpr int ICON_COUNT = 5;
		const float rightClusterW = ICON_W * ICON_COUNT + ICON_GAP * (ICON_COUNT - 1);
		ImGui::SetCursorPos(ImVec2(barW - rightClusterW - 8.0f, (barH - ICON_W) * 0.5f));

		if (SynthIconButton(SynthIcon::Settings, "##synth_settings", "Settings"))
			mainWindow->OpenSettingsDialog();
		ImGui::SameLine(0.0f, ICON_GAP);
		if (SynthIconButton(SynthIcon::Search, "##synth_open", "Open MIDI"))
			mainWindow->OpenMIDIDialog();
		ImGui::SameLine(0.0f, ICON_GAP);
		if (SynthIconButton(SynthIcon::Counter, "##synth_counter", "Show note counter"))
			config->render.showCounter = !config->render.showCounter;
		ImGui::SameLine(0.0f, ICON_GAP);
		if (SynthIconButton(SynthIcon::Fullscreen, "##synth_fullscreen", "Fullscreen"))
			mainWindow->ToggleFullscreen();
		ImGui::SameLine(0.0f, ICON_GAP);
		const bool menuClicked = SynthIconButton(SynthIcon::Menu, "##synth_menu", "Menu");

		// --- centre: transport and zoom ------------------------------------------------------
		constexpr float BUTTON_W = 24.0f;
		constexpr float TRANSPORT_W = 32.0f * 3.0f + 2.0f * 2.0f;
		constexpr float ZOOM_W = BUTTON_W * 2.0f + 66.0f + 12.0f;
		const float centerW = TRANSPORT_W + 16.0f + ZOOM_W;
		ImGui::SetCursorPos(ImVec2((barW - centerW) * 0.5f, (barH - 30.0f) * 0.5f));

		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.09f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 1.0f, 1.0f, 0.16f));

		const bool playing = timer->IsRunning() && !timer->IsPaused();

		if (SynthTransportButton(playing ? SynthTransport::Pause : SynthTransport::Play,
			"##synth_play", playing ? "Pause" : "Play"))
		{
			if (playing) timer->Pause();
			else timer->Resume();
		}

		ImGui::SameLine(0.0f, 2.0f);
		if (SynthTransportButton(SynthTransport::Back, "##synth_back", "Back"))
		{
			const float targetTime = std::max((float)MIN_MIDI_TIME_SECS,
				(float)(timer->Elapsed() - config->navigation.seekBackwardSeconds));
			timer->NavigateTo(targetTime);
		}

		ImGui::SameLine(0.0f, 2.0f);
		if (SynthTransportButton(SynthTransport::Forward, "##synth_forward", "Forward"))
		{
			const float targetTime = std::min((float)midiLength,
				(float)(timer->Elapsed() + config->navigation.seekForwardSeconds));
			timer->NavigateTo(targetTime);
		}

		ImGui::SameLine(0.0f, 16.0f);

		// zoom out
		if (ImGui::Button("-", ImVec2(BUTTON_W, 28.0f)))
		{
			long next = std::lround(renderView->viewTicks / 1.25);
			renderView->viewTicks = std::clamp(next, 48L, 7680L);
		}
		ImGui::SameLine(0.0f, 6.0f);

		// zoom percentage + tempo readout
		{
			const ImVec2 textOrigin = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(66.0f, 28.0f));

			const int zoomPercent = static_cast<int>(std::lround((double)renderView->viewTicks / 1280.0 * 100.0));
			char percentText[16];
			std::snprintf(percentText, sizeof(percentText), "%d%%", zoomPercent);

			double bpm = 0.0;
			if (MIDISequence* seq = app->GetRendererSequence())
			{
				if (TempoMap* tempoMap = seq->GetTempoMap())
				{
					long tick = 0;
					if (!seq->timeBased && seq->resolution > 0)
						tick = tempoMap->SecsToTicksFromMap(seq->resolution, timer->Elapsed());
					bpm = tempoMap->GetBPMAtTick(std::max(0L, tick));
				}
			}

			char bpmText[32];
			if (bpm > 0.0) std::snprintf(bpmText, sizeof(bpmText), "%.0f BPM", bpm);
			else std::snprintf(bpmText, sizeof(bpmText), "-- BPM");

			const ImVec2 pctSize = ImGui::CalcTextSize(percentText);
			dl->AddText(ImVec2(textOrigin.x + (66.0f - pctSize.x) * 0.5f, textOrigin.y + 0.5f),
				textCol, percentText);

			const ImVec2 bpmSize = ImGui::CalcTextSize(bpmText);
			const float smallScale = 11.0f / ImGui::GetFontSize();
			dl->AddText(ImGui::GetFont(), 11.0f,
				ImVec2(textOrigin.x + (66.0f - bpmSize.x * smallScale) * 0.5f, textOrigin.y + 16.0f),
				ImGui::GetColorU32(SYNTH_TEXT_DIM), bpmText);
		}

		ImGui::SameLine(0.0f, 6.0f);

		// zoom in
		if (ImGui::Button("+", ImVec2(BUTTON_W, 28.0f)))
		{
			long next = std::lround(renderView->viewTicks * 1.25);
			renderView->viewTicks = std::clamp(next, 48L, 7680L);
		}

		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar();

		// --- seek line along the bottom edge -------------------------------------------------
		const float seekX0 = pos.x + 8.0f;
		const float seekX1 = pos.x + barW - 8.0f;
		const float seekY = pos.y + barH - 3.0f;
		const double safeLength = std::max(1.0, midiLength);
		const double clampedTime = std::clamp((double)timer->Elapsed(), 0.0, safeLength);
		const float progress = (float)std::clamp(clampedTime / safeLength, 0.0, 1.0);

		dl->AddRectFilled(ImVec2(seekX0, seekY), ImVec2(seekX1, seekY + 2.0f), ImGui::GetColorU32(SYNTH_SEEK_BG));
		dl->AddRectFilled(ImVec2(seekX0, seekY), ImVec2(seekX0 + (seekX1 - seekX0) * progress, seekY + 2.0f),
			ImGui::GetColorU32(SYNTH_SEEK_PROGRESS));

		ImGui::SetCursorScreenPos(ImVec2(seekX0, seekY - 5.0f));
		ImGui::InvisibleButton("##synth_seek", ImVec2(seekX1 - seekX0, 10.0f));
		if (ImGui::IsItemActive())
		{
			const float t = std::clamp((ImGui::GetIO().MousePos.x - seekX0) / (seekX1 - seekX0), 0.0f, 1.0f);
			timer->NavigateTo(t * safeLength);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seek");

		// --- menu popup ----------------------------------------------------------------------
		if (menuClicked) ImGui::OpenPopup("##synth_main_menu");
		if (ImGui::BeginPopup("##synth_main_menu"))
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("Open...")) mainWindow->OpenMIDIDialog();
				if (ImGui::MenuItem("Render to Video...")) mainWindow->OpenRenderVideoDialog();
				if (ImGui::MenuItem("Diagnose MIDI...")) mainWindow->OpenDiagnosisDialog();
				ImGui::Separator();
				if (ImGui::MenuItem("Quit"))
					glfwSetWindowShouldClose(mainWindow->GetInternalWindow(), GLFW_TRUE);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Play"))
			{
				if (ImGui::MenuItem("Stop")) timer->Stop();
				if (ImGui::MenuItem("Stop and Unload"))
				{
					timer->Stop();
					app->UnloadMIDI();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View"))
			{
				ImGui::MenuItem("Show note counter", nullptr, &config->render.showCounter);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Options"))
			{
				if (ImGui::MenuItem("Settings...")) mainWindow->OpenSettingsDialog();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Help"))
			{
				if (ImGui::MenuItem("Buy ponluxime a coffee"))
					Utils::OpenURL("https://ko-fi.com/ponluxime");
				if (ImGui::MenuItem("Check for Updates")) mainWindow->OpenUpdateDialog();
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
	}
	ImGui::End();
	ImGui::PopStyleColor(6);
	ImGui::PopStyleVar(3);
}

void NavigationBar::Update()
{

}

#undef NAVIGATION_ROWS
#undef MIN_MIDI_TIME_SECS
