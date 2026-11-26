#include "ScoreOverlay.h"

#include "GPUImage.h"
#include "App/Fonts.h"
#include "App/MIDIApp.h"
#include "App/UI/NavigationBar.h"
#include "MIDI/TempoMap.h"
#include "MIDI/Timer/MIDITimer.h"
#include "Utils.h"
#include "Config/MIDIPlayerConfig.h"
#include "MIDI/MIDISequence.h"

#include <cfloat>

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
	constexpr int SHEET_LETTER[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };

	// MIDI 0 is C-1, MIDI 60 is C4. The diatonic number counts staff steps from C-1, which
	// keeps the ledger-line maths independent of the clef.
	int DiatonicNumber(int midiNote)
	{
		return (midiNote / 12 - 1) * 7 + SHEET_LETTER[midiNote % 12];
	}

	// Staff position of a MIDI note. In flat keys a black key is written as the flat above it
	// (B-flat on the B line, not A-sharp on the A line).
	int StaffStep(int midiNote, int base, bool flatKey)
	{
		int step = DiatonicNumber(midiNote) - base;
		if (flatKey && IsSharp((size_t)midiNote)) ++step;
		return step;
	}

	// Pitch classes of the naturals the key signature alters, in signature order.
	constexpr int SHARP_PCS[7] = { 5, 0, 7, 2, 9, 4, 11 };  // F C G D A E B
	constexpr int FLAT_PCS[7] = { 11, 4, 9, 2, 7, 0, 5 };   // B E A D G C F

	// Where those accidentals sit on each staff, counted from the bottom staff line.
	constexpr int TREBLE_SHARP_STEPS[7] = { 8, 5, 10, 6, 3, 7, 4 };
	constexpr int BASS_SHARP_STEPS[7] = { 6, 3, 7, 4, 1, 5, 2 };
	constexpr int TREBLE_FLAT_STEPS[7] = { 4, 7, 3, 6, 2, 5, 1 };
	constexpr int BASS_FLAT_STEPS[7] = { 9, 5, 8, 4, 7, 10, 6 };

	struct NoteGlyph
	{
		float x = 0.0f;
		float y = 0.0f;
		uint32_t tick = 0;
		uint32_t gate = 0;
		uint32_t key = 0;
		bool treble = true;
		bool active = false;
	};

	struct Onset
	{
		uint32_t tick = 0;
		bool treble = true;
		float x = 0.0f;
		uint32_t maxGate = 0;
		uint32_t minGate = UINT32_MAX;
		std::vector<size_t> glyphs;
	};
}

void ScoreOverlay::LoadTextures()
{
	texturesLoaded = true;
	sheetTreble = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/treble.png"));
	sheetBass = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/bass.png"));
	sheetSharp = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/sharp.png"));
	sheetFlat = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/flat.png"));
	sheetNatural = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/natural.png"));
	noteBlack = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/notehead-black.png"));
	noteHalf = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/notehead-white.png"));
	noteWhole = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/notehead-whole.png"));
	flagUp = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/flag-up.png"));
	flagDown = std::make_unique<GPUImage>(Utils::TryGetStream("./assets/textures/synthesia/sheet/flag-down.png"));
}

void ScoreOverlay::DrawGlyph(GPUImage* image, float centerX, float centerY, float glyphHeight, uint32_t tint)
{
	if (!image || !image->IsValidTexture() || image->height <= 0)
		return;

	const float glyphWidth = glyphHeight * (float)image->width / (float)image->height;
	ImGui::GetBackgroundDrawList()->AddImage(
		(ImTextureID)(intptr_t)image->GetTextureID(),
		ImVec2(centerX - glyphWidth * 0.5f, centerY - glyphHeight * 0.5f),
		ImVec2(centerX + glyphWidth * 0.5f, centerY + glyphHeight * 0.5f),
		// GPUImage uploads the first PNG row as the OpenGL bottom row.
		ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), (ImU32)tint);
}

void ScoreOverlay::DrawGlyphAnchored(GPUImage* image, float left, float top, float glyphHeight, uint32_t tint)
{
	if (!image || !image->IsValidTexture() || image->height <= 0)
		return;

	const float glyphWidth = glyphHeight * (float)image->width / (float)image->height;
	ImGui::GetBackgroundDrawList()->AddImage(
		(ImTextureID)(intptr_t)image->GetTextureID(),
		ImVec2(left, top),
		ImVec2(left + glyphWidth, top + glyphHeight),
		ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), (ImU32)tint);
}

void ScoreOverlay::Render()
{
	if (!app) return;

	MIDIPlayerConfig* config = app->GetConfig();
	if (!config || !config->render.showScore) return;

	MIDISequence* seq = app->GetRendererSequence();
	if (!seq || seq->timeBased || seq->mergedNotes.size() < MIDI_KEYS) return;
	if (!app->IsGLReady()) return;

	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const float uiWidth = display.x;
	const float uiHeight = display.y;
	if (uiWidth < 64.0f || uiHeight < 64.0f) return;

	TempoMap* tempoMap = seq->GetTempoMap();
	if (!tempoMap || !app->GetTimer()) return;

	if (!texturesLoaded) LoadTextures();

	const double elapsed = std::max(0.0, app->GetTimer()->Elapsed());
	const long currentTick = tempoMap->SecsToTicksFromMap(seq->resolution, elapsed);

	int numerator = 4;
	int denominator = 4;
	if (!seq->timeSignatures.empty())
	{
		const TimeSignatureEvent& signature = seq->timeSignatures.front();
		if (signature.numerator > 0) numerator = signature.numerator;
		if (signature.denominator > 0) denominator = signature.denominator;
	}

	const long measureTicks = std::max(1L, (long)seq->resolution * 4L * numerator / denominator);
	const long windowTicks = measureTicks * 4L;
	// Keep the playhead near the middle of the four visible measures, like a score that scrolls
	// under a fixed cursor. At the start the window is clamped so the first notes are visible.
	long windowStart = std::max(0L, currentTick - windowTicks / 2);
	const long windowEnd = windowStart + windowTicks;

	// The navigation bar is chrome above the renderer; live previews start the page below it.
	// Video renders have no navigation bar and use the full frame.
	float panelTop = 0.0f;
	if (!app->IsRendering())
	{
		if (NavigationBar* navigationBar = app->GetNavigationBar())
			panelTop = std::max(0.0f, navigationBar->GetPosition().y + navigationBar->GetResolution().y);
	}

	const float availableHeight = std::max(150.0f, uiHeight - panelTop);
	const float panelHeight = std::clamp(availableHeight * 0.38f, 200.0f, 500.0f);
	const float panelBottom = panelTop + panelHeight;

	// The reference score uses a large staff: the two five-line staves together fill most of the
	// page. Scale the spacing from the panel height, then shrink it if the grand staff would not
	// fit with margins.
	float space = std::clamp(panelHeight / 15.0f, 4.0f, 30.0f);
	float staffBlock = space * 8.9f;
	const float maxBlock = panelHeight * 0.84f;
	if (staffBlock > maxBlock)
	{
		space = maxBlock / 8.9f;
		staffBlock = space * 8.9f;
	}

	const float trebleTop = panelTop + (panelHeight - staffBlock) * 0.5f + space * 0.35f;
	const float trebleBottom = trebleTop + space * 4.0f;
	const float bassTop = trebleBottom + space * 0.9f;
	const float bassBottom = bassTop + space * 4.0f;

	const float staffLeft = std::max(8.0f, uiWidth * 0.018f);
	const float staffRight = uiWidth - std::max(8.0f, uiWidth * 0.018f);

	// Key signature at the visible start. MIDI carries the number of sharps/flats directly.
	int keySharpsFlats = 0;
	for (const KeySignatureEvent& signature : seq->keySignatures)
	{
		if (signature.tick <= windowStart)
			keySharpsFlats = signature.sharpsFlats;
		else
			break;
	}
	keySharpsFlats = std::clamp(keySharpsFlats, -7, 7);
	const int accidentalCount = std::abs(keySharpsFlats);
	const bool flatKey = keySharpsFlats < 0;

	bool inKey[12] = { false };
	inKey[0] = inKey[2] = inKey[4] = inKey[5] = inKey[7] = inKey[9] = inKey[11] = true;
	// A natural note whose pitch class the signature alters needs a natural sign when it
	// appears (B natural in F major, F natural in G major, and so on).
	bool keyAltered[12] = { false };
	for (int i = 0; i < accidentalCount; ++i)
	{
		keyAltered[flatKey ? FLAT_PCS[i] : SHARP_PCS[i]] = true;
		if (flatKey)
			inKey[(FLAT_PCS[i] + 11) % 12] = true;
		else
			inKey[(SHARP_PCS[i] + 1) % 12] = true;
	}

	const float keyX = staffLeft + space * 4.6f;
	const float keyStep = space * 0.95f;
	const float timeX = keyX + accidentalCount * keyStep + (accidentalCount > 0 ? space * 0.35f : 0.0f);
	const float contentLeft = std::max(staffLeft + space * 7.0f, timeX + space * 2.5f);
	const float contentWidth = std::max(1.0f, staffRight - contentLeft);

	const ImU32 paper = IM_COL32(252, 252, 250, 255);
	const ImU32 paperEdge = IM_COL32(203, 203, 198, 255);
	const ImU32 ink = IM_COL32(25, 25, 25, 255);
	const ImU32 staffInk = IM_COL32(38, 38, 38, 255);
	const ImU32 cursorInk = IM_COL32(28, 28, 28, 210);
	const ImU32 activeInk = IM_COL32(216, 38, 38, 255);
	const ImU32 glyphTint = 0xFFFFFFFFu;

	ImDrawList* draw = ImGui::GetBackgroundDrawList();

	draw->AddRectFilled(ImVec2(0.0f, panelTop), ImVec2(uiWidth, panelBottom), paper);
	draw->AddLine(ImVec2(0.0f, panelBottom), ImVec2(uiWidth, panelBottom), paperEdge, 1.0f);

	draw->PushClipRect(ImVec2(0.0f, panelTop), ImVec2(uiWidth, panelBottom), true);

	auto staffLines = [&](float top, float bottom)
		{
			for (int line = 0; line < 5; ++line)
			{
				const float y = top + (bottom - top) * (float)line / 4.0f;
				draw->AddLine(ImVec2(staffLeft, y), ImVec2(staffRight, y), staffInk, std::max(1.0f, space * 0.045f));
			}
		};

	staffLines(trebleTop, trebleBottom);
	staffLines(bassTop, bassBottom);

	// Grand staff bracket.
	draw->AddLine(ImVec2(staffLeft, trebleTop), ImVec2(staffLeft, bassBottom), staffInk, std::max(1.0f, space * 0.11f));

	// Clefs are the Wikimedia Commons GClef/FClef glyphs, scaled to the staff.
	DrawGlyph(sheetTreble.get(), staffLeft + space * 1.0f, trebleTop + space * 1.8f, space * 5.6f, glyphTint);
	DrawGlyph(sheetBass.get(), staffLeft + space * 1.7f, bassTop + space * 2.0f, space * 3.9f, glyphTint);

	// Key signature on both staves.
	if (accidentalCount > 0)
	{
		GPUImage* accidental = flatKey ? sheetFlat.get() : sheetSharp.get();
		const int* trebleSteps = flatKey ? TREBLE_FLAT_STEPS : TREBLE_SHARP_STEPS;
		const int* bassSteps = flatKey ? BASS_FLAT_STEPS : BASS_SHARP_STEPS;
		const float accidentalHeight = space * 2.0f;

		for (int i = 0; i < accidentalCount; ++i)
		{
			const float x = keyX + i * keyStep;
			DrawGlyph(accidental, x, trebleBottom - trebleSteps[i] * space * 0.5f, accidentalHeight, glyphTint);
			DrawGlyph(accidental, x, bassBottom - bassSteps[i] * space * 0.5f, accidentalHeight, glyphTint);
		}
	}

	// Time signature, after the key signature.
	ImFont* font = Fonts::DefaultFont != nullptr ? Fonts::DefaultFont : ImGui::GetFont();
	if (font != nullptr)
	{
		const float signatureSize = space * 1.9f;
		const std::string numeratorText = std::to_string(numerator);
		const std::string denominatorText = std::to_string(denominator);
		draw->AddText(font, signatureSize, ImVec2(timeX, trebleTop - space * 0.15f), ink, numeratorText.c_str());
		draw->AddText(font, signatureSize, ImVec2(timeX, trebleTop + space * 1.85f), ink, denominatorText.c_str());
	}

	// Barlines follow the real measure grid, so an anacrusis or a time-signature change does not
	// get an evenly divided but wrong measure width.
	const long firstMeasure = windowStart / measureTicks;
	for (long m = firstMeasure; ; ++m)
	{
		const long measureStart = m * measureTicks;
		if (measureStart > windowEnd) break;
		if (measureStart <= windowStart) continue;

		const float x = contentLeft + (float)((double)(measureStart - windowStart) / (double)windowTicks) * contentWidth;
		if (x < staffLeft || x > staffRight) continue;

		draw->AddLine(ImVec2(x, trebleTop), ImVec2(x, trebleBottom), staffInk, std::max(1.0f, space * 0.085f));
		draw->AddLine(ImVec2(x, bassTop), ImVec2(x, bassBottom), staffInk, std::max(1.0f, space * 0.085f));
	}

	// Notes, grouped per key by the loader, are flattened into one tick-ordered list so chords
	// can share a stem and runs can share beams.
	const long quarterTicks = std::max(1L, (long)seq->resolution);
	std::vector<NoteGlyph> glyphs;
	glyphs.reserve(1024);
	constexpr size_t MAX_SHEET_NOTES = 2400;

	for (int key = 0; key < 128 && glyphs.size() < MAX_SHEET_NOTES; ++key)
	{
		const NoteSequence& notes = seq->mergedNotes[key];
		if (notes.Empty()) continue;

		const bool treble = key >= 60;
		const int base = treble ? (4 * 7 + 2) : (2 * 7 + 4); // E4 / G2, the bottom staff lines.
		const float staffBottom = treble ? trebleBottom : bassBottom;
		const int diatonicStep = StaffStep(key, base, flatKey);

		auto begin = std::lower_bound(notes.tick.begin(), notes.tick.end(), (uint32_t)windowStart);
		size_t index = (size_t)std::distance(notes.tick.begin(), begin);
		uint32_t previousTick = UINT32_MAX;
		uint32_t previousGate = UINT32_MAX;

		for (; index < notes.Size() && glyphs.size() < MAX_SHEET_NOTES; ++index)
		{
			const uint32_t tick = notes.tick[index];
			if (tick >= (uint32_t)windowEnd) break;
			if (tick == previousTick && notes.gate[index] == previousGate) continue;
			previousTick = tick;
			previousGate = notes.gate[index];

			const float x = contentLeft + (float)((double)((long)tick - windowStart) / (double)windowTicks) * contentWidth;
			if (x < staffLeft || x > staffRight) continue;

			const uint32_t gate = std::max<uint32_t>(1, notes.gate[index]);
			NoteGlyph glyph;
			glyph.x = x;
			glyph.y = staffBottom - (float)diatonicStep * space * 0.5f;
			glyph.tick = tick;
			glyph.gate = gate;
			glyph.key = (uint32_t)key;
			glyph.treble = treble;
			glyph.active = (long)tick <= currentTick && currentTick < (long)tick + (long)gate;
			glyphs.push_back(glyph);
		}
	}

	std::sort(glyphs.begin(), glyphs.end(), [](const NoteGlyph& a, const NoteGlyph& b)
		{
			if (a.tick != b.tick) return a.tick < b.tick;
			if (a.treble != b.treble) return a.treble > b.treble;
			return a.key < b.key;
		});

	std::vector<Onset> onsets;
	for (size_t i = 0; i < glyphs.size(); ++i)
	{
		if (onsets.empty() || onsets.back().tick != glyphs[i].tick || onsets.back().treble != glyphs[i].treble)
		{
			Onset onset;
			onset.tick = glyphs[i].tick;
			onset.treble = glyphs[i].treble;
			onset.x = glyphs[i].x;
			onsets.push_back(onset);
		}

		Onset& onset = onsets.back();
		onset.glyphs.push_back(i);
		onset.maxGate = std::max(onset.maxGate, glyphs[i].gate);
		onset.minGate = std::min(onset.minGate, glyphs[i].gate);
	}

	const float headRX = space * 0.50f;
	const float stemWidth = std::max(1.0f, space * 0.075f);
	const float cursorX = contentLeft + (float)((double)(currentTick - windowStart) / (double)windowTicks) * contentWidth;

	// Playback cursor, like the vertical line in the reference score.
	if (cursorX >= staffLeft && cursorX <= staffRight)
	{
		draw->AddLine(ImVec2(cursorX, trebleTop - space * 1.7f), ImVec2(cursorX, bassBottom + space * 1.7f),
			cursorInk, std::max(1.0f, space * 0.07f));
	}

	// A run of short notes inside one beat is beamed. This is what makes sixteenth-note
	// passages read like the reference instead of a row of separate flags.
	struct BeamSpan
	{
		size_t firstOnset = 0;
		size_t lastOnset = 0;
		bool stemUp = true;
		float beamY = 0.0f;
		int beamCount = 1;
	};

	std::vector<BeamSpan> beams;
	std::vector<bool> beamed(glyphs.size(), false);

	const float trebleCenter = (trebleTop + trebleBottom) * 0.5f;
	const float bassCenter = (bassTop + bassBottom) * 0.5f;

	for (size_t i = 0; i < onsets.size(); )
	{
		const float staffCenter = onsets[i].treble ? trebleCenter : bassCenter;
		size_t j = i;
		float ySum = 0.0f;
		int yCount = 0;
		bool shortNotes = true;

		auto accumulate = [&](size_t onsetIndex)
			{
				for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				{
					ySum += glyphs[glyphIndex].y;
					++yCount;
				}
				if (onsets[onsetIndex].maxGate >= (uint32_t)quarterTicks)
					shortNotes = false;
			};

		accumulate(i);
		while (j + 1 < onsets.size() && shortNotes)
		{
			const Onset& next = onsets[j + 1];
			if (next.treble != onsets[i].treble) break;
			if (next.tick - onsets[j].tick > (uint32_t)quarterTicks) break;
			if (next.tick / (uint32_t)quarterTicks != onsets[i].tick / (uint32_t)quarterTicks) break;
			if (j - i >= 7) break;

			accumulate(j + 1);
			++j;
		}

		if (j > i && shortNotes && yCount > 0)
		{
			BeamSpan span;
			span.firstOnset = i;
			span.lastOnset = j;
			const float averageY = ySum / (float)yCount;
			span.stemUp = averageY > staffCenter;

			float beamY = span.stemUp ? FLT_MAX : -FLT_MAX;
			int beamCount = 1;
			for (size_t k = i; k <= j; ++k)
			{
				for (size_t glyphIndex : onsets[k].glyphs)
				{
					const NoteGlyph& glyph = glyphs[glyphIndex];
					beamY = span.stemUp ? std::min(beamY, glyph.y) : std::max(beamY, glyph.y);
					if (glyph.gate < (uint32_t)(quarterTicks / 2)) beamCount = 2;
				}
			}
			span.beamY = span.stemUp ? beamY - space * 3.0f : beamY + space * 3.0f;
			span.beamCount = beamCount;
			beams.push_back(span);

			for (size_t k = i; k <= j; ++k)
				for (size_t glyphIndex : onsets[k].glyphs)
					beamed[glyphIndex] = true;

			i = j + 1;
		}
		else
		{
			++i;
		}
	}

	for (const BeamSpan& beam : beams)
	{
		const Onset& first = onsets[beam.firstOnset];
		const Onset& last = onsets[beam.lastOnset];

		for (size_t k = beam.firstOnset; k <= beam.lastOnset; ++k)
		{
			const Onset& onset = onsets[k];
			float attachY = beam.stemUp ? FLT_MAX : -FLT_MAX;
			for (size_t glyphIndex : onset.glyphs)
			{
				const NoteGlyph& glyph = glyphs[glyphIndex];
				attachY = beam.stemUp ? std::min(attachY, glyph.y) : std::max(attachY, glyph.y);
			}

			const float stemX = onset.x + (beam.stemUp ? headRX : -headRX);
			draw->AddLine(ImVec2(stemX, attachY), ImVec2(stemX, beam.beamY), ink, stemWidth);
		}

		for (int line = 0; line < beam.beamCount; ++line)
		{
			const float y = beam.beamY + (beam.stemUp ? -1.0f : 1.0f) * (float)line * space * 0.34f;
			const float startX = first.x + (beam.stemUp ? headRX : -headRX);
			const float endX = last.x + (beam.stemUp ? headRX : -headRX);
			draw->AddLine(ImVec2(startX, y), ImVec2(endX, y), ink, std::max(1.0f, space * 0.13f));
		}
	}

	// Unbeamed onsets share one stem and one flag, including chords. Drawing a stem per
	// notehead would fan flags out of every chord tone.
	for (const Onset& onset : onsets)
	{
		if (onset.glyphs.empty() || beamed[onset.glyphs.front()])
			continue;

		float topY = FLT_MAX;
		float bottomY = -FLT_MAX;
		float ySum = 0.0f;
		bool anyStemmed = false;
		for (size_t glyphIndex : onset.glyphs)
		{
			const NoteGlyph& glyph = glyphs[glyphIndex];
			topY = std::min(topY, glyph.y);
			bottomY = std::max(bottomY, glyph.y);
			ySum += glyph.y;
			if (glyph.gate < (uint32_t)quarterTicks * 4)
				anyStemmed = true;
		}
		if (!anyStemmed)
			continue;

		const float averageY = ySum / (float)onset.glyphs.size();
		const bool stemUp = averageY > (onset.treble ? trebleCenter : bassCenter);
		const float stemX = onset.x + (stemUp ? headRX : -headRX);
		const float attachY = stemUp ? topY : bottomY;
		const float stemTip = attachY + (stemUp ? -space * 3.0f : space * 3.0f);
		draw->AddLine(ImVec2(stemX, attachY), ImVec2(stemX, stemTip), ink, stemWidth);

		if (onset.minGate < (uint32_t)quarterTicks)
		{
			const int flags = onset.minGate < (uint32_t)(quarterTicks / 2) ? 2 : 1;
			const float flagHeight = space * 2.0f;
			for (int flag = 0; flag < flags; ++flag)
			{
				const float offset = (float)flag * space * 0.42f;
				if (stemUp)
					DrawGlyphAnchored(flagUp.get(), stemX, stemTip + offset, flagHeight, glyphTint);
				else
					DrawGlyphAnchored(flagDown.get(), stemX, stemTip - flagHeight - offset, flagHeight, glyphTint);
			}
		}
	}

	for (size_t i = 0; i < glyphs.size(); ++i)
	{
		const NoteGlyph& glyph = glyphs[i];
		const bool openHead = glyph.gate >= (uint32_t)quarterTicks * 2;
		const bool wholeHead = glyph.gate >= (uint32_t)quarterTicks * 4;
		const ImVec2 center(glyph.x, glyph.y);

		const int trebleBase = 4 * 7 + 2;
		const int bassBase = 2 * 7 + 4;
		const int base = glyph.treble ? trebleBase : bassBase;
		const float staffBottom = glyph.treble ? trebleBottom : bassBottom;
		const int diatonicStep = StaffStep((int)glyph.key, base, flatKey);

		// Ledger lines only on line positions, never in the space next to the staff.
		if (diatonicStep <= -2)
		{
			for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep - 1; step < 0; step += 2)
			{
				const float ledgerY = staffBottom - (float)step * space * 0.5f;
				draw->AddLine(ImVec2(glyph.x - headRX * 1.6f, ledgerY), ImVec2(glyph.x + headRX * 1.6f, ledgerY),
					staffInk, std::max(1.0f, space * 0.075f));
			}
		}
		else if (diatonicStep >= 10)
		{
			for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep + 1; step > 8; step -= 2)
			{
				const float ledgerY = staffBottom - (float)step * space * 0.5f;
				draw->AddLine(ImVec2(glyph.x - headRX * 1.6f, ledgerY), ImVec2(glyph.x + headRX * 1.6f, ledgerY),
					staffInk, std::max(1.0f, space * 0.075f));
			}
		}

		// Accidental only when the note is outside the key signature, or when a natural note
		// cancels a key-signature accidental. Flat keys spell black keys as flats and sharp
		// keys as sharps, matching how the score is engraved.
		const uint32_t pitchClass = glyph.key % 12;
		const bool sharpNote = IsSharp((size_t)glyph.key);
		const bool naturalSign = !sharpNote && keyAltered[pitchClass];
		const bool altered = sharpNote && !inKey[pitchClass];
		if (altered || naturalSign)
		{
			const float accidentalHeight = space * 2.2f;
			GPUImage* accidental = naturalSign ? sheetNatural.get() : (flatKey ? sheetFlat.get() : sheetSharp.get());
			const float accidentalWidth = accidental && accidental->height > 0
				? accidentalHeight * (float)accidental->width / (float)accidental->height
				: space * 1.2f;
			DrawGlyph(accidental, glyph.x - headRX - accidentalWidth * 0.5f - space * 0.2f, glyph.y,
				accidentalHeight, glyphTint);
		}

		const float noteheadHeight = space * 0.92f;
		GPUImage* notehead = wholeHead ? noteWhole.get() : (openHead ? noteHalf.get() : noteBlack.get());
		DrawGlyph(notehead, glyph.x, glyph.y, noteheadHeight, glyphTint);

		// The reference marks sounding notes with a red dot at the cursor.
		if (glyph.active)
			draw->AddEllipseFilled(center, ImVec2(space * 0.17f, space * 0.17f), activeInk, 0.0f, 12);
	}

	draw->PopClipRect();
}
