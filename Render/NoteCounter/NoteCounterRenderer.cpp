#include "NoteCounterRenderer.h"
#include "App/Fonts.h"
#include "App/FontList.h"
#include "imgui.h"
#include <string>
#include <cfloat>
#include "Utils.h"
#include "App/MIDIApp.h"
#include "MIDI/MIDIDefs.h"
#include "NoteMultiplier.h"

static void BeginNextCounterRow(const char* label)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::Text(label);
	ImGui::TableSetColumnIndex(1);
}

bool NoteCounterRenderer::IsShown()
{
	MIDIPlayerConfig* config = app->GetConfig();
	return config->render.showCounter;
}

void NoteCounterRenderer::Render(float heightOffset)
{
	MIDIPlayerConfig* config = app->GetConfig();
	ImVec4 bgColor = config->overlayInfo.backgroundCol;
	ImVec4 txtColor = config->overlayInfo.textCol;

	NoteCounterAlignment counterAlignment = config->overlayInfo.overlayAlignment;
	NoteCounterStyle counterStyle = config->overlayInfo.overlayStyle;

	// font magic
	ImFont* fontToUse = nullptr;
	for (const auto& f : app->GetFontList()->GetFonts())
	{
		if (f.path == config->overlayInfo.selectedFontPath)
		{
			fontToUse = f.font;
			break;
		}
	}

	lastCounterYOffset = heightOffset;

	// Which counted notes, NPS and polyphony belong to tracks the multiplier leaves alone. MIDIApp
	// measures this once per frame (see UpdateOverlayRates) so the Graph overlay shows exactly the
	// same numbers these rows do, with no chance of the two drifting apart.
	playedPlainNotes = app->GetPlainPlayedNotes();
	plainNotesPerSecond = app->GetPlainNotesPerSecond();
	plainPolyphonyNotes = app->GetPlainPolyphony();

	if (fontToUse) ImGui::PushFont(fontToUse);
	else ImGui::PushFont(Fonts::MonoFont);

	float counterScale = config->overlayInfo.scale;
	float minCounterWidth = this->counterWidth * counterScale;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f * counterScale, 5.0f * counterScale));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, bgColor);
	ImGui::PushStyleColor(ImGuiCol_Text, txtColor);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(-10.0f, 2.0f));

	ApplyCounterAlignment(heightOffset);

	
	switch (counterStyle)
	{
		case NoteCounterStyle::UMP:
			RenderUMP(counterScale);
			break;
		case NoteCounterStyle::MIDITrail:
			RenderMIDITrail(counterScale);
			break;
		default: break;
	}

	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);
	ImGui::PopFont();
}

void NoteCounterRenderer::RenderUMP(float counterScale)
{
	MIDIPlayerConfig* config = app->GetConfig();
	float minCounterWidth = this->counterWidth * counterScale;

	if (ImGui::Begin("noteCounterUMP", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::SetWindowFontScale(counterScale);

		// unfortunate that i have to completely change how i calculate counter width, else it fucks with the overall counter width
		// hence why it was wider than usual when the scake is 1.0 :/

		struct CounterRow { const char* label; char value[512]; };
		CounterRow rows[6];
		int rowCount = 0;

		if (noteCounterInfo->tick.shown)
		{
			auto ticks = noteCounterInfo->tick.value;
			OverlayUtils::FormatText(rows[rowCount].value, "%s/%u", Utils::FormatWithCommas(ticks > 0 ? ticks : 0).c_str(), noteCounterInfo->ppq.value);
			rows[rowCount].label = "Tick";
			rowCount++;
		}
		if (noteCounterInfo->timeSeconds.shown)
		{
			OverlayUtils::FormatText(rows[rowCount].value, "%s", Utils::FormatDuration2(noteCounterInfo->timeSeconds.value * 1000).c_str());
			rows[rowCount].label = "Time";
			rowCount++;
		}
		if (noteCounterInfo->bpm.shown)
		{
			OverlayUtils::FormatText(rows[rowCount].value, "%.1f", noteCounterInfo->bpm.value);
			rows[rowCount].label = "BPM";
			rowCount++;
		}
		if (noteCounterInfo->notesPassed.shown)
		{
			OverlayUtils::FormatText(rows[rowCount].value, "%s", NoteMultiplier::FormatCount(config, noteCounterInfo->notesPassed.value, playedPlainNotes).c_str());
			rows[rowCount].label = "Notes";
			rowCount++;
		}
		if (noteCounterInfo->notesPerSecond.shown)
		{
			OverlayUtils::FormatText(rows[rowCount].value, "%s", NoteMultiplier::FormatBoosted(config, noteCounterInfo->notesPerSecond.value, noteCounterInfo->notesPassed.value, plainNotesPerSecond, playedPlainNotes).c_str());
			rows[rowCount].label = "NPS";
			rowCount++;
		}
		if (noteCounterInfo->polyphony.shown)
		{
			OverlayUtils::FormatText(rows[rowCount].value, "%s", NoteMultiplier::FormatBoosted(config, noteCounterInfo->polyphony.value, noteCounterInfo->notesPassed.value, plainPolyphonyNotes, playedPlainNotes).c_str());
			rows[rowCount].label = "Polyphony";
			rowCount++;
		}

		/*if (!app->IsRendering())
		{
			if (noteCounterInfo->fps.shown)
			{
				OverlayUtils::FormatText(rows[rowCount].value, "%.1f", noteCounterInfo->fps.value);
				rows[rowCount].label = "FPS";
				rowCount++;
			}

			if (noteCounterInfo->audioBuffer.shown)
			{
				double buffer = noteCounterInfo->audioBuffer.value;
				OverlayUtils::FormatText(rows[rowCount].value, buffer < -0.5 ? "N/A" : Utils::FormatDuration2(buffer * 1000).c_str(), buffer);
				rows[rowCount].label = "Buffer";
				rowCount++;
			}
		}*/
		

		float labelColWidth = 0.0f, valueColWidth = 0.0f;
		for (int i = 0; i < rowCount; i++)
		{
			labelColWidth = std::max(labelColWidth, ImGui::CalcTextSize(rows[i].label).x);
			valueColWidth = std::max(valueColWidth, ImGui::CalcTextSize(rows[i].value).x);
		}

		float windowPaddingX = 5.0f * counterScale;
		// The CellPadding pushed in Render() is negative, so it can't be used as spacing; keep a
		// positive gap so the value column is never narrower than the count it has to right-align.
		float naturalContentWidth = labelColWidth + valueColWidth + 8.0f * counterScale;
		float minContentWidth = std::max(0.0f, minCounterWidth - windowPaddingX * 2.0f);
		float tableWidth = ClampCounterWidth(std::max(minContentWidth, naturalContentWidth), windowPaddingX);

		if (ImGui::BeginTable("counterStats", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoClip, ImVec2(tableWidth, 0.0f)))
		{
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoClip);

			for (int i = 0; i < rowCount; i++)
			{
				BeginNextCounterRow(rows[i].label);
				OverlayUtils::RightAlignedTableText(rows[i].value);
			}

			ImGui::EndTable();
		}

		lastCounterWidth = ImGui::GetWindowWidth();
		lastCounterHeight = ImGui::GetWindowHeight();

		ImGui::SetWindowFontScale(1.0f);
	}
	ImGui::End();
}

void NoteCounterRenderer::RenderMIDITrail(float counterScale)
{
	MIDIPlayerConfig* config = app->GetConfig();
	float minCounterWidth = this->counterWidth * counterScale;

	// The MIDITrail counter is a single line, so a big multiplied count can stretch it past the
	// render target. Cap the window at the viewport width and let the line wrap instead.
	float windowPaddingX = 5.0f * counterScale;
	float maxWindowWidth = ClampCounterWidth(FLT_MAX, windowPaddingX) + windowPaddingX * 2.0f;
	if (maxWindowWidth > 0.0f)
		ImGui::SetNextWindowSizeConstraints(ImVec2(minCounterWidth, 0.0f), ImVec2(maxWindowWidth, FLT_MAX));

	if (ImGui::Begin("noteCounterMIDITrail", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::SetWindowFontScale(counterScale);

		std::stringstream stats;
		
		bool hasFirstField = false;

		if (noteCounterInfo->tick.shown)
		{
			stats << "TICK:" << Utils::FormatWithCommas(noteCounterInfo->tick.value) << "/" << noteCounterInfo->ppq.value;
			hasFirstField = true;
		}

		if (noteCounterInfo->timeSeconds.shown)
		{
			if (hasFirstField) stats << "  ";
			stats << "TIME:" << Utils::FormatDuration2(noteCounterInfo->timeSeconds.value * 1000);
			hasFirstField = true;
		}

		if (noteCounterInfo->bpm.shown)
		{
			if (hasFirstField) stats << "  ";
			stats << "BPM:" << std::fixed << std::setprecision(1) << noteCounterInfo->bpm.value;
			hasFirstField = true;
		}

		if (noteCounterInfo->notesPassed.shown)
		{
			if (hasFirstField) stats << "  ";
			stats << "NOTES:" << NoteMultiplier::FormatCount(config, noteCounterInfo->notesPassed.value, playedPlainNotes);
			hasFirstField = true;
		}

		if (noteCounterInfo->notesPerSecond.shown)
		{
			if (hasFirstField) stats << "  ";
			stats << "NPS:" << NoteMultiplier::FormatBoosted(config, noteCounterInfo->notesPerSecond.value, noteCounterInfo->notesPassed.value, plainNotesPerSecond, playedPlainNotes);
			hasFirstField = true;
		}

		if (noteCounterInfo->polyphony.shown)
		{
			if (hasFirstField) stats << "  ";
			stats << "POLY:" << NoteMultiplier::FormatBoosted(config, noteCounterInfo->polyphony.value, noteCounterInfo->notesPassed.value, plainPolyphonyNotes, playedPlainNotes);
		}

		float wrapRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		ImGui::PushTextWrapPos(wrapRight);
		ImGui::TextWrapped("%s", stats.str().c_str());
		ImGui::PopTextWrapPos();

		lastCounterWidth = ImGui::GetWindowWidth();
		lastCounterHeight = ImGui::GetWindowHeight();

		ImGui::SetWindowFontScale(1.0f);
	}
	ImGui::End();
}

float NoteCounterRenderer::ClampCounterWidth(float desiredWidth, float windowPaddingX) const
{
	ImGuiIO& io = ImGui::GetIO();
	float viewportWidth = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : (float)this->width;
	if (viewportWidth <= 0.0f)
		return desiredWidth;

	const float margin = 4.0f;
	float maxWidth = viewportWidth - windowPaddingX * 2.0f - margin * 2.0f;
	return std::min(desiredWidth, std::max(0.0f, maxWidth));
}

void NoteCounterRenderer::ApplyCounterAlignment(float heightOffset)
{
	// Anchor the window to the edge picked in Settings, using the same convention as
	// GetOverlayPosition (and the blur behind it). Anchoring the *edge* means a wide multiplied
	// count grows inward and stays on screen, instead of relying on a remembered drag position
	// that can end up anywhere.
	MIDIPlayerConfig* config = app->GetConfig();
	float viewportWidth = ImGui::GetIO().DisplaySize.x;
	if (viewportWidth <= 0.0f)
		viewportWidth = (float)this->width;

	ImVec2 pivot(0.0f, 0.0f);
	float anchorX = 0.0f;

	switch (config->overlayInfo.overlayAlignment)
	{
		case NoteCounterAlignment::TopCenter:
			anchorX = viewportWidth * 0.5f;
			pivot = ImVec2(0.5f, 0.0f);
			break;
		case NoteCounterAlignment::TopRight:
			anchorX = viewportWidth;
			pivot = ImVec2(1.0f, 0.0f);
			break;
		case NoteCounterAlignment::TopLeft:
		default:
			anchorX = 0.0f;
			pivot = ImVec2(0.0f, 0.0f);
			break;
	}

	ImGui::SetNextWindowPos(ImVec2(anchorX, heightOffset), ImGuiCond_Always, pivot);
}

void NoteCounterRenderer::OnResize(int width, int height)
{
	this->width = width;
	this->height = height;
}

float NoteCounterRenderer::GetCounterHeight() const
{
	return lastCounterHeight;
}

glm::vec2 NoteCounterRenderer::GetOverlayPosition() const
{
	MIDIPlayerConfig* config = app->GetConfig();
	NoteCounterAlignment counterAlignment = config->overlayInfo.overlayAlignment;

	float width = (float)lastCounterWidth / (float)this->width;
	float height = GetCounterHeight() / (float)this->height;

	float yPos = 1.0 - height - lastCounterYOffset / (float)this->height;
	switch (counterAlignment)
	{
		case NoteCounterAlignment::TopLeft:
		{
			return glm::vec2(0.0, yPos);
		}
		case NoteCounterAlignment::TopCenter:
		{
			return glm::vec2(0.5f - width * 0.5f, yPos);
		}
		case NoteCounterAlignment::TopRight:
		{
			return glm::vec2(1.0 - width, yPos);
		}
		default:
			return glm::vec2(0.0f);
	}
}

glm::vec2 NoteCounterRenderer::GetOverlaySize() const
{
	float width = lastCounterWidth / (float)this->width;
	float height = GetCounterHeight() / (float)this->height;
	return glm::vec2(width, height);
}