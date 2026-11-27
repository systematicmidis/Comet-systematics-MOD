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
		uint32_t value = 0;
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
		uint32_t rhythm = 0;
		uint32_t value = 0;
		std::vector<size_t> glyphs;
	};
}

void ScoreOverlay::LoadTextures()
{
	texturesLoaded = true;
	const char* base = "./assets/textures/synthesia/bravura/";
	sheetTreble = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "clef-treble.png"));
	sheetBass = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "clef-bass.png"));
	sheetSharp = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "accidental-sharp.png"));
	sheetFlat = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "accidental-flat.png"));
	sheetNatural = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "accidental-natural.png"));
	sheetBrace = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "brace.png"));
	noteBlack = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "notehead-black.png"));
	noteHalf = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "notehead-half.png"));
	noteWhole = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "notehead-whole.png"));
	sheetRestWhole = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "rest-whole.png"));
	flagUp = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-up-8.png"));
	flagDown = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-down-8.png"));
	flagUp16 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-up-16.png"));
	flagDown16 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-down-16.png"));
	flagUp32 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-up-32.png"));
	flagDown32 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-down-32.png"));
	flagUp64 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-up-64.png"));
	flagDown64 = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "flag-down-64.png"));
	for (int i = 0; i < 10; ++i)
		sheetTime[i] = std::make_unique<GPUImage>(Utils::TryGetStream(std::string(base) + "time-" + std::to_string(i) + ".png"));
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
		ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), (ImU32)tint);
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
		ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), (ImU32)tint);
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
	const long pageTicks = measureTicks * 4L;
	// Plus2 lays the score out as fixed four-measure pages: the cursor sweeps across a page
	// and the page turns when it reaches the right edge. It does not slide continuously.
	const long windowStart = (currentTick / pageTicks) * pageTicks;
	const long windowEnd = windowStart + pageTicks;

	// The score is a panel over the top half of the window, leaving the piano roll visible
	// underneath instead of covering the complete frame.
	float pageTop = 0.0f;
	if (!app->IsRendering())
	{
		if (NavigationBar* navigationBar = app->GetNavigationBar())
			pageTop = std::max(0.0f, navigationBar->GetPosition().y + navigationBar->GetResolution().y);
	}
	const float availableHeight = std::max(160.0f, uiHeight - pageTop);
	// The panel height is configurable (Settings > Visual > Renderer > Sheet music size).
	// The notation is derived from it below, so a taller panel engraves a larger page
	// instead of adding blank paper around a fixed-size staff.
	const float scoreFraction = (float)std::clamp(config->render.scoreScale, 15, 100) / 100.0f;
	const float pageHeight = availableHeight * scoreFraction;
	const float pageBottom = pageTop + pageHeight;

	// Plus2 draws one staff for treble-only MIDI and a grand staff for piano MIDI. Use the
	// same rule so the test-file layout and Bad Apple's two-staff layout both match.
	bool hasBassNotes = false;
	for (int key = 0; key < 60 && !hasBassNotes; ++key)
	{
		if (!seq->mergedNotes[key].Empty())
			hasBassNotes = true;
	}

	// Plus2's staff spacing is about 1.1% of the window height at the default half-screen
	// panel, and the grand staff leaves roughly six staff spaces between its two staves.
	const float space = std::clamp(pageHeight * 0.0220f, 3.0f, 30.0f);
	const float staffGap = space * 5.9f;
	// Even a treble-only score keeps the treble staff where it sits in a grand staff, so the
	// page position matches Plus2's single-staff view.
	const float staffBlock = space * 13.9f;
	const float blockTop = pageTop + pageHeight * 0.5f - staffBlock * 0.5f;
	const float trebleTop = blockTop;
	const float trebleBottom = trebleTop + space * 4.0f;
	const float bassTop = trebleBottom + staffGap;
	const float bassBottom = bassTop + space * 4.0f;

	const float staffLeft = std::max(8.0f, uiWidth * 0.023f);
	const float staffRight = uiWidth - std::max(6.0f, uiWidth * 0.003f);

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

	const float keyX = staffLeft + space * 3.0f;
	const float keyStep = space * 1.5f;
	float timeX = staffLeft + space * 10.4f;
	if (accidentalCount > 0)
		timeX = std::max(timeX, keyX + (accidentalCount - 1) * keyStep + space * 1.0f);
	const float contentLeft = std::max(staffLeft + space * 13.6f, timeX + space * 2.6f);
	const float contentWidth = std::max(1.0f, staffRight - contentLeft);
	// Plus2 insets the notes from the measure grid: barlines sit at the grid, while the first
	// notehead of each measure sits about three staff spaces to its right.
	const float noteInset = std::min(space * 3.0f, contentWidth * 0.25f);
	const float noteLeft = contentLeft + noteInset;
	const float noteWidth = std::max(1.0f, contentWidth - noteInset);
	const float cursorX = noteLeft + (float)((double)(currentTick - windowStart) / (double)pageTicks) * noteWidth;

	const ImU32 paper = IM_COL32(255, 255, 255, 255);
	const ImU32 ink = IM_COL32(0, 0, 0, 255);
	const ImU32 staffInk = IM_COL32(0, 0, 0, 255);
	const ImU32 cursorInk = IM_COL32(0, 0, 0, 255);
	const ImU32 activeInk = IM_COL32(255, 0, 0, 255);
	const ImU32 glyphTint = 0xFFFFFFFFu;

	ImDrawList* draw = ImGui::GetBackgroundDrawList();

	draw->AddRectFilled(ImVec2(0.0f, pageTop), ImVec2(uiWidth, pageBottom), paper);
	draw->AddLine(ImVec2(0.0f, pageBottom), ImVec2(uiWidth, pageBottom), IM_COL32(120, 120, 120, 90), 1.0f);

	draw->PushClipRect(ImVec2(0.0f, pageTop), ImVec2(uiWidth, pageBottom), true);

	auto staffLines = [&](float top, float bottom)
		{
			for (int line = 0; line < 5; ++line)
			{
				const float y = top + (bottom - top) * (float)line / 4.0f;
				draw->AddLine(ImVec2(staffLeft, y), ImVec2(staffRight, y), staffInk, std::max(1.0f, space * 0.045f));
			}
		};

	staffLines(trebleTop, trebleBottom);
	if (hasBassNotes)
		staffLines(bassTop, bassBottom);

	// Plus2 joins the grand staff with a curly brace. The Bravura brace is a scalable SMuFL glyph.
	if (hasBassNotes)
	{
		const float braceHeight = space * 20.0f;
		const float braceTop = trebleTop - space * 3.0f;
		DrawGlyphAnchored(sheetBrace.get(), staffLeft - space * 1.9f, braceTop, braceHeight, glyphTint);
	}

	// Bravura clefs, the same font Plus2 engraves with.
	DrawGlyph(sheetTreble.get(), staffLeft + space * 0.4f, trebleTop + space * 2.0f, space * 6.4f, glyphTint);
	if (hasBassNotes)
		DrawGlyph(sheetBass.get(), staffLeft + space * 0.4f, bassTop + space * 2.0f, space * 3.2f, glyphTint);

	// Key signature on every shown staff.
	if (accidentalCount > 0)
	{
		GPUImage* accidental = flatKey ? sheetFlat.get() : sheetSharp.get();
		const int* trebleSteps = flatKey ? TREBLE_FLAT_STEPS : TREBLE_SHARP_STEPS;
		const int* bassSteps = flatKey ? BASS_FLAT_STEPS : BASS_SHARP_STEPS;
		const float accidentalHeight = space * 2.5f;

		for (int i = 0; i < accidentalCount; ++i)
		{
			const float x = keyX + i * keyStep;
			DrawGlyph(accidental, x, trebleBottom - trebleSteps[i] * space * 0.5f, accidentalHeight, glyphTint);
			if (hasBassNotes)
				DrawGlyph(accidental, x, bassBottom - bassSteps[i] * space * 0.5f, accidentalHeight, glyphTint);
		}
	}

	// Time signature uses Bravura's music digits, not the UI font.
	const std::string numeratorText = std::to_string(numerator);
	const std::string denominatorText = std::to_string(denominator);
	auto drawTimeDigits = [&](const std::string& text, float x, float top, float height)
		{
			float cursor = x;
			for (char c : text)
			{
				if (c < '0' || c > '9') continue;
				GPUImage* digit = sheetTime[c - '0'].get();
				DrawGlyphAnchored(digit, cursor, top, height, glyphTint);
				if (digit && digit->height > 0)
					cursor += height * (float)digit->width / (float)digit->height + space * 0.06f;
			}
		};
	auto drawTimeSignature = [&](float staffTop)
		{
			drawTimeDigits(numeratorText, timeX, staffTop - space * 0.9f, space * 2.7f);
			drawTimeDigits(denominatorText, timeX, staffTop + space * 2.5f, space * 2.7f);
		};
	drawTimeSignature(trebleTop);
	if (hasBassNotes)
		drawTimeSignature(bassTop);

	// Barlines follow the real measure grid, so an anacrusis or a time-signature change does not
	// get an evenly divided but wrong measure width.
	const long firstMeasure = windowStart / measureTicks;
	for (long m = firstMeasure; ; ++m)
	{
		const long measureStart = m * measureTicks;
		if (measureStart > windowEnd) break;
		if (measureStart <= windowStart) continue;

		const float x = contentLeft + (float)((double)(measureStart - windowStart) / (double)pageTicks) * contentWidth;
		if (x < staffLeft || x > staffRight) continue;

		draw->AddLine(ImVec2(x, trebleTop), ImVec2(x, trebleBottom), staffInk, std::max(1.0f, space * 0.085f));
		if (hasBassNotes)
			draw->AddLine(ImVec2(x, bassTop), ImVec2(x, bassBottom), staffInk, std::max(1.0f, space * 0.085f));
	}

	// Notes, grouped per key by the loader, are flattened into one tick-ordered list so chords
	// can share a stem and runs can share beams.
	const long quarterTicks = std::max(1L, (long)seq->resolution);
	std::vector<NoteGlyph> glyphs;
	glyphs.reserve(4096);
	// A dense four-measure page can hold thousands of notes. Keep enough headroom that the
	// visible page is complete instead of silently truncating the right-hand measures.
	constexpr size_t MAX_SHEET_NOTES = 20000;

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

		for (; index < notes.Size() && glyphs.size() < MAX_SHEET_NOTES; ++index)
		{
			const uint32_t tick = notes.tick[index];
			if (tick >= (uint32_t)windowEnd) break;

			// Bad Apple carries the same piano part on several tracks. The loader merges them
			// into one per-key list, so skip repeated tick/key pairs (keeping the longest gate)
			// or the score grows double noteheads, stems, flags, and beams.
			if (tick == previousTick)
			{
				if (!glyphs.empty() && glyphs.back().key == (uint32_t)key)
					glyphs.back().gate = std::max(glyphs.back().gate, (uint32_t)std::max<uint32_t>(1, notes.gate[index]));
				continue;
			}
			previousTick = tick;

			const float x = noteLeft + (float)((double)((long)tick - windowStart) / (double)pageTicks) * noteWidth;
			if (x < staffLeft || x > staffRight) continue;

			const uint32_t gate = std::max<uint32_t>(1, notes.gate[index]);
			NoteGlyph glyph;
			glyph.x = x;
			glyph.y = staffBottom - (float)diatonicStep * space * 0.5f;
			glyph.tick = tick;
			glyph.gate = gate;
			glyph.value = gate;
			glyph.key = (uint32_t)key;
			glyph.treble = treble;
			// Plus2's red dot rides the cursor, not every sustained note on the page.
			const bool recentAttack = currentTick - (long)tick <= std::max(1L, quarterTicks / 8);
			const bool underCursor = std::abs(x - cursorX) <= space * 0.8f;
			glyph.active = currentTick > windowStart && (long)tick <= currentTick && currentTick < (long)tick + (long)gate
				&& (recentAttack || underCursor);
			glyphs.push_back(glyph);
		}
	}

	std::sort(glyphs.begin(), glyphs.end(), [](const NoteGlyph& a, const NoteGlyph& b)
		{
			if (a.tick != b.tick) return a.tick < b.tick;
			if (a.treble != b.treble) return a.treble > b.treble;
			return a.key < b.key;
		});

	// Plus2 engraves a whole rest in every empty measure of each shown staff.
	bool measureHasTreble[4] = { false, false, false, false };
	bool measureHasBass[4] = { false, false, false, false };
	for (const NoteGlyph& glyph : glyphs)
	{
		const long measure = ((long)glyph.tick - windowStart) / measureTicks;
		if (measure < 0 || measure >= 4) continue;
		if (glyph.treble)
			measureHasTreble[measure] = true;
		else
			measureHasBass[measure] = true;
	}

	auto drawWholeRest = [&](float staffTop, int measure)
		{
			const float centerX = contentLeft + ((float)measure + 0.5f) / 4.0f * contentWidth;
			// A whole rest hangs from the fourth line, not from the second.
			DrawGlyph(sheetRestWhole.get(), centerX, staffTop + space * 3.55f, space * 0.58f, glyphTint);
		};

	for (int measure = 0; measure < 4; ++measure)
	{
		if (!measureHasTreble[measure])
			drawWholeRest(trebleTop, measure);
		if (hasBassNotes && !measureHasBass[measure])
			drawWholeRest(bassTop, measure);
	}

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

	// Beam and tuplet runs are per staff. In piano music both hands often attack at the same
	// tick, so walking the globally interleaved onset list would break every run at the other
	// hand's onset.
	std::vector<size_t> trebleOnsets;
	std::vector<size_t> bassOnsets;
	for (size_t i = 0; i < onsets.size(); ++i)
		(onsets[i].treble ? trebleOnsets : bassOnsets).push_back(i);
	const std::vector<size_t> onsetSequences[2] = { trebleOnsets, bassOnsets };

	// A MIDI note's gate is how long it is held, not how the note is written. Legato playing
	// often holds an eighth note for a whole quarter, which used to suppress its beam and leave
	// it looking like a long note. Use the distance to the next onset on the same staff as the
	// notated value, which is what a score reader expects to see.
	for (const std::vector<size_t>& sequence : onsetSequences)
	{
		for (size_t p = 0; p < sequence.size(); ++p)
		{
			Onset& onset = onsets[sequence[p]];
			uint32_t rhythm = onset.minGate;
			if (p + 1 < sequence.size())
				rhythm = onsets[sequence[p + 1]].tick - onset.tick;
			if (rhythm == 0 || rhythm == UINT32_MAX)
				rhythm = std::max<uint32_t>(1, onset.minGate == UINT32_MAX ? 1 : onset.minGate);
			onset.rhythm = rhythm;
			onset.minGate = rhythm;
			onset.maxGate = rhythm;
			onset.value = rhythm;
			for (size_t glyphIndex : onset.glyphs)
			{
				glyphs[glyphIndex].gate = rhythm;
				glyphs[glyphIndex].value = rhythm;
			}
		}
	}

	const float headRX = space * 0.6f;
	const float stemWidth = std::max(1.0f, space * 0.075f);

	// Number of beams/flags a note value needs: 8th = 1, 16th = 2, 32nd = 3, 64th = 4.
	auto beamLevel = [&](uint32_t gate)
		{
			if (gate < (uint32_t)std::max(1L, quarterTicks / 8)) return 4;
			if (gate < (uint32_t)std::max(1L, quarterTicks / 4)) return 3;
			if (gate < (uint32_t)std::max(1L, quarterTicks / 2)) return 2;
			if (gate < (uint32_t)quarterTicks) return 1;
			return 0;
		};

	// Traditional stem direction: the note farthest from the middle line decides the chord.
	auto onsetStemUp = [&](const Onset& onset, float staffCenter)
		{
			float farthest = -1.0f;
			bool below = false;
			for (size_t glyphIndex : onset.glyphs)
			{
				const float distance = std::abs(glyphs[glyphIndex].y - staffCenter);
				if (distance > farthest)
				{
					farthest = distance;
					below = glyphs[glyphIndex].y > staffCenter;
				}
			}
			return below;
		};

	// Plus2's cursor is a thin black line down the whole page.
	if (currentTick > windowStart && cursorX >= staffLeft && cursorX <= staffRight)
	{
		draw->AddLine(ImVec2(cursorX, pageTop), ImVec2(cursorX, pageBottom),
			cursorInk, std::max(1.0f, space * 0.12f));
	}

	// A run of short notes inside one beat is beamed. This is what makes sixteenth-note
	// passages read like the reference instead of a row of separate flags.
	struct BeamSpan
	{
		std::vector<size_t> members;   // onsets in rhythm order, one staff only
		float firstX = 0.0f;
		float slope = 0.0f;
		float beamStartY = 0.0f;
		float beamEndY = 0.0f;
		bool stemUp = true;
		int beamCount = 1;
	};

	std::vector<BeamSpan> beams;
	std::vector<bool> beamed(glyphs.size(), false);
	std::vector<int> onsetBeam(onsets.size(), -1);

	const float trebleCenter = (trebleTop + trebleBottom) * 0.5f;
	const float bassCenter = (bassTop + bassBottom) * 0.5f;

	auto onsetOuterY = [&](size_t onsetIndex, bool stemUp)
		{
			float y = stemUp ? FLT_MAX : -FLT_MAX;
			for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				y = stemUp ? std::min(y, glyphs[glyphIndex].y) : std::max(y, glyphs[glyphIndex].y);
			return y;
		};

	// The end of the stem opposite its tip. A chord is engraved with one stem that runs
	// through every notehead, so the line starts at this note and extends past the outer
	// one. Attaching it to the outer note instead left the other chord tones floating.
	auto onsetAnchorY = [&](size_t onsetIndex, bool stemUp)
		{
			float y = stemUp ? -FLT_MAX : FLT_MAX;
			for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				y = stemUp ? std::max(y, glyphs[glyphIndex].y) : std::min(y, glyphs[glyphIndex].y);
			return y;
		};

	// Tuplets are inferred from timing before anything is beamed. MIDI has no tuplet flag,
	// so three equal-spaced notes whose combined span equals two normal notes are read as a
	// triplet. A triplet is written with the value it replaces (an eighth triplet takes one
	// beam), not with the shorter value its sounding gap suggests.
	struct TupletSpan
	{
		size_t first = 0;
		size_t second = 0;
		size_t third = 0;
		bool treble = true;
	};
	std::vector<TupletSpan> tuplets;
	const long tupletTolerance = std::max(2L, quarterTicks / 24);
	const long tupletUnits[4] = { quarterTicks / 2, quarterTicks, quarterTicks * 2, quarterTicks * 4 };

	for (const std::vector<size_t>& sequence : onsetSequences)
	{
		for (size_t p = 0; p + 2 < sequence.size(); )
		{
			const size_t i0 = sequence[p];
			const size_t i1 = sequence[p + 1];
			const size_t i2 = sequence[p + 2];
			const long firstGap = (long)onsets[i1].tick - (long)onsets[i0].tick;
			const long secondGap = (long)onsets[i2].tick - (long)onsets[i1].tick;
			bool valid = firstGap > 0 && std::abs(firstGap - secondGap) <= tupletTolerance;

			if (valid)
			{
				const long unit = firstGap * 3;
				bool unitMatches = false;
				for (long candidate : tupletUnits)
					if (candidate > 0 && std::abs(unit - candidate) <= tupletTolerance * 2)
						unitMatches = true;
				valid = unitMatches;
			}

			if (!valid)
			{
				++p;
				continue;
			}

			// Exactly three notes per bracket. A longer triplet-spaced run is engraved as
			// consecutive triplets instead of one bracket over six or nine notes.
			TupletSpan span;
			span.first = i0;
			span.second = i1;
			span.third = i2;
			span.treble = onsets[i0].treble;
			tuplets.push_back(span);

			const uint32_t written = std::max<uint32_t>(1, (uint32_t)((firstGap * 3 + 1) / 2));
			for (size_t k = p; k < p + 3; ++k)
			{
				onsets[sequence[k]].value = written;
				for (size_t glyphIndex : onsets[sequence[k]].glyphs)
					glyphs[glyphIndex].value = written;
			}
			p += 3;
		}
	}

	for (const std::vector<size_t>& sequence : onsetSequences)
	{
		for (size_t pos = 0; pos < sequence.size(); )
		{
			const size_t firstIndex = sequence[pos];
			if (beamLevel(onsets[firstIndex].value) == 0)
			{
				++pos;
				continue;
			}

			// Collect one beat of short notes from this staff only. Walking the global onset
			// table here used to pull in the other hand's notes, and every one of them grew a
			// long stem across the staff, fusing whole measures into black bars.
			std::vector<size_t> members;
			members.push_back(firstIndex);
			while (pos + 1 < sequence.size() && members.size() < 8)
			{
				const size_t nextIndex = sequence[pos + 1];
				const Onset& previous = onsets[members.back()];
				const Onset& next = onsets[nextIndex];
				if (beamLevel(next.value) == 0) break;
				if (next.tick - previous.tick > (uint32_t)quarterTicks) break;
				if (next.tick / (uint32_t)quarterTicks != onsets[firstIndex].tick / (uint32_t)quarterTicks) break;
				members.push_back(nextIndex);
				++pos;
			}

			if (members.size() < 2)
			{
				++pos;
				continue;
			}

			const float staffCenter = onsets[firstIndex].treble ? trebleCenter : bassCenter;
			BeamSpan span;
			span.members = members;
			float farthest = -1.0f;
			bool below = false;
			for (size_t onsetIndex : members)
			{
				for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				{
					const float distance = std::abs(glyphs[glyphIndex].y - staffCenter);
					if (distance > farthest)
					{
						farthest = distance;
						below = glyphs[glyphIndex].y > staffCenter;
					}
				}
			}
			span.stemUp = below;

			const size_t lastIndex = members.back();
			const float firstOuter = onsetOuterY(firstIndex, span.stemUp);
			const float lastOuter = onsetOuterY(lastIndex, span.stemUp);
			const float dx = std::max(1.0f, onsets[lastIndex].x - onsets[firstIndex].x);
			// Keep the beam nearly level. A slope taken from a wide leap turns a dense trill
			// into a diagonal black wedge.
			const float slope = std::clamp((lastOuter - firstOuter) / dx, -0.12f, 0.12f);
			span.firstX = onsets[firstIndex].x;
			span.slope = slope;
			span.beamStartY = span.stemUp ? firstOuter - space * 3.3f : firstOuter + space * 3.3f;
			span.beamEndY = span.beamStartY + slope * dx;

			int beamCount = 1;
			for (size_t onsetIndex : members)
				beamCount = std::max(beamCount, beamLevel(onsets[onsetIndex].value));
			span.beamCount = beamCount;

			// Lift the whole beam until every note in this group clears it.
			for (size_t onsetIndex : members)
			{
				const float at = span.beamStartY + slope * (onsets[onsetIndex].x - span.firstX);
				const float outer = onsetOuterY(onsetIndex, span.stemUp);
				if (span.stemUp && outer - space * 2.8f < at)
				{
					const float lift = at - (outer - space * 2.8f);
					span.beamStartY -= lift;
					span.beamEndY -= lift;
				}
				else if (!span.stemUp && outer + space * 2.8f > at)
				{
					const float lift = (outer + space * 2.8f) - at;
					span.beamStartY += lift;
					span.beamEndY += lift;
				}
			}

			// Keep an inward-pointing beam inside its own staff. Treble stems-down beams and
			// bass stems-up beams otherwise meet in the gap between the staves and read as one
			// cross-staff black bar.
			if (!span.stemUp && onsets[firstIndex].treble)
			{
				const float limit = trebleBottom + space * 2.2f;
				bool safe = true;
				for (size_t onsetIndex : members)
					if (onsetOuterY(onsetIndex, false) > limit)
						safe = false;
				if (safe)
				{
					span.beamStartY = std::min(span.beamStartY, limit);
					span.beamEndY = std::min(span.beamEndY, limit);
				}
			}
			else if (span.stemUp && !onsets[firstIndex].treble)
			{
				const float limit = bassTop - space * 2.2f;
				bool safe = true;
				for (size_t onsetIndex : members)
					if (onsetOuterY(onsetIndex, true) < limit)
						safe = false;
				if (safe)
				{
					span.beamStartY = std::max(span.beamStartY, limit);
					span.beamEndY = std::max(span.beamEndY, limit);
				}
			}

			const int beamIndex = (int)beams.size();
			beams.push_back(span);
			for (size_t onsetIndex : members)
			{
				onsetBeam[onsetIndex] = beamIndex;
				for (size_t glyphIndex : onsets[onsetIndex].glyphs)
					beamed[glyphIndex] = true;
			}

			++pos;
		}
	}

	for (const BeamSpan& beam : beams)
	{
		auto beamYAt = [&](float x) { return beam.beamStartY + beam.slope * (x - beam.firstX); };
		const Onset& first = onsets[beam.members.front()];
		const Onset& last = onsets[beam.members.back()];

		for (size_t onsetIndex : beam.members)
		{
			const Onset& onset = onsets[onsetIndex];
			const float attachY = onsetAnchorY(onsetIndex, beam.stemUp);
			const float stemX = onset.x + (beam.stemUp ? headRX : -headRX);
			draw->AddLine(ImVec2(stemX, attachY), ImVec2(stemX, beamYAt(stemX)), ink, stemWidth);
		}

		// Engraving thickness: half a staff space per beam, a quarter space between beams.
		// The old thin bars with a wide gap read as separate lines instead of one beam.
		const float beamThickness = std::max(1.5f, space * 0.5f);
		const float beamStep = std::max(2.0f, space * 0.75f);
		for (int line = 0; line < beam.beamCount; ++line)
		{
			const float offset = (float)line * beamStep;
			auto drawBeamSegment = [&](float startX, float endX)
				{
					const float y0 = beamYAt(startX) + (beam.stemUp ? offset : -offset);
					const float y1 = beamYAt(endX) + (beam.stemUp ? offset : -offset);
					draw->AddQuadFilled(ImVec2(startX, y0), ImVec2(endX, y1),
						ImVec2(endX, y1 + beamThickness), ImVec2(startX, y0 + beamThickness), ink);
				};

			if (line == 0)
			{
				drawBeamSegment(first.x + (beam.stemUp ? headRX : -headRX),
					last.x + (beam.stemUp ? headRX : -headRX));
				continue;
			}

			// Extra beams only cover the notes that need them, and only the notes in this
			// group: an eighth inside a sixteenth group keeps one beam.
			for (size_t m = 0; m < beam.members.size(); )
			{
				if (beamLevel(onsets[beam.members[m]].value) <= line)
				{
					++m;
					continue;
				}

				const size_t runStart = m;
				while (m + 1 < beam.members.size() && beamLevel(onsets[beam.members[m + 1]].value) > line)
					++m;
				const size_t runEnd = m;

				float startX = onsets[beam.members[runStart]].x + (beam.stemUp ? headRX : -headRX);
				float endX = onsets[beam.members[runEnd]].x + (beam.stemUp ? headRX : -headRX);
				if (runEnd == runStart)
				{
					const float stub = space * 0.9f;
					if (runStart + 1 < beam.members.size())
						endX = startX + stub;
					else
						endX = startX - stub;
				}
				drawBeamSegment(startX, endX);
				++m;
			}
		}
	}

	// Phrase slurs. MIDI has no phrase marks, so each staff's continuous melodic motion is
	// grouped into phrases: a rest longer than a half note, a page-long run or a very wide
	// leap ends the phrase. The slur sits on the notehead side, opposite the stems.
	struct PhraseSpan
	{
		std::vector<size_t> members;
		bool above = true;
	};
	std::vector<PhraseSpan> phrases;

	auto onsetAverageY = [&](size_t onsetIndex)
		{
			float sum = 0.0f;
			for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				sum += glyphs[glyphIndex].y;
			return sum / (float)onsets[onsetIndex].glyphs.size();
		};
	auto onsetTopBottom = [&](size_t onsetIndex, float& top, float& bottom)
		{
			top = FLT_MAX;
			bottom = -FLT_MAX;
			for (size_t glyphIndex : onsets[onsetIndex].glyphs)
			{
				top = std::min(top, glyphs[glyphIndex].y);
				bottom = std::max(bottom, glyphs[glyphIndex].y);
			}
		};

	for (const std::vector<size_t>& sequence : onsetSequences)
	{
		for (size_t pos = 0; pos < sequence.size(); )
		{
			std::vector<size_t> members;
			members.push_back(sequence[pos]);
			float rangeTop = FLT_MAX;
			float rangeBottom = -FLT_MAX;
			onsetTopBottom(sequence[pos], rangeTop, rangeBottom);
			while (pos + 1 < sequence.size() && members.size() < 10)
			{
				const Onset& previous = onsets[members.back()];
				const Onset& next = onsets[sequence[pos + 1]];
				if (next.tick - previous.tick > (uint32_t)quarterTicks * 2) break;
				if (next.tick - onsets[members.front()].tick > (uint32_t)measureTicks) break;
				if (std::abs(onsetAverageY(sequence[pos + 1]) - onsetAverageY(members.back())) > space * 5.0f)
					break;

				// A phrase mark follows a melodic line, not a run that sweeps across the whole
				// staff; keep the slur close enough to arch over its own notes.
				float nextTop = FLT_MAX;
				float nextBottom = -FLT_MAX;
				onsetTopBottom(sequence[pos + 1], nextTop, nextBottom);
				if (std::max(rangeBottom, nextBottom) - std::min(rangeTop, nextTop) > space * 4.0f)
					break;

				rangeTop = std::min(rangeTop, nextTop);
				rangeBottom = std::max(rangeBottom, nextBottom);
				members.push_back(sequence[pos + 1]);
				++pos;
			}
			++pos;

			if (members.size() < 4)
				continue;
			const float phraseWidth = onsets[members.back()].x - onsets[members.front()].x;
			if (phraseWidth < space * 3.5f)
				continue;

			// Fast scalar and trill runs are beamed figures, not lyrical phrases. Slurring
			// them only produces a long diagonal through the staff.
			const float averageGap = (float)(onsets[members.back()].tick - onsets[members.front()].tick)
				/ (float)(members.size() - 1);
			if (averageGap < (float)quarterTicks * 0.25f)
				continue;

			// A phrase is a melodic line, not a chordal accompaniment or a repeated-note
			// trill (which is normally marked "tr" rather than slurred).
			size_t singleNotes = 0;
			float sumY = 0.0f;
			size_t noteCount = 0;
			for (size_t onsetIndex : members)
			{
				if (onsets[onsetIndex].glyphs.size() == 1)
					++singleNotes;
				for (size_t glyphIndex : onsets[onsetIndex].glyphs)
				{
					sumY += glyphs[glyphIndex].y;
					++noteCount;
				}
			}
			if (singleNotes * 5 < members.size() * 3)
				continue;
			if (members.size() >= 8 && rangeBottom - rangeTop < space * 1.6f)
				continue;

			int stemsUp = 0;
			int stemsDown = 0;
			for (size_t onsetIndex : members)
			{
				if (onsetBeam[onsetIndex] < 0)
					continue;
				if (beams[onsetBeam[onsetIndex]].stemUp)
					++stemsUp;
				else
					++stemsDown;
			}

			const float staffCenter = onsets[members.front()].treble ? trebleCenter : bassCenter;
			PhraseSpan phrase;
			phrase.members = members;
			// More stems down means the slur is engraved above, and vice versa.
			phrase.above = stemsUp != stemsDown
				? stemsDown > stemsUp
				: (sumY / (float)std::max<size_t>(1, noteCount)) <= staffCenter;
			phrases.push_back(phrase);
		}
	}

	for (const PhraseSpan& phrase : phrases)
	{
		const Onset& firstOnset = onsets[phrase.members.front()];
		const Onset& lastOnset = onsets[phrase.members.back()];
		float firstTop = FLT_MAX;
		float firstBottom = -FLT_MAX;
		float lastTop = FLT_MAX;
		float lastBottom = -FLT_MAX;
		for (size_t glyphIndex : firstOnset.glyphs)
		{
			firstTop = std::min(firstTop, glyphs[glyphIndex].y);
			firstBottom = std::max(firstBottom, glyphs[glyphIndex].y);
		}
		for (size_t glyphIndex : lastOnset.glyphs)
		{
			lastTop = std::min(lastTop, glyphs[glyphIndex].y);
			lastBottom = std::max(lastBottom, glyphs[glyphIndex].y);
		}

		const float startX = firstOnset.x;
		const float endX = lastOnset.x;
		const float span = std::max(space * 1.0f, endX - startX);
		const float arch = std::clamp(span * 0.055f, space * 1.3f, space * 2.8f);
		bool above = phrase.above;
		float startY = above ? firstTop - space * 0.9f : firstBottom + space * 0.9f;
		float endY = above ? lastTop - space * 0.9f : lastBottom + space * 0.9f;
		// Do not let a wide phrase turn the slur into a straight diagonal.
		endY = std::clamp(endY, startY - space * 2.0f, startY + space * 2.0f);

		// Keep the arc inside the page; flip it only when it would be clipped.
		if (above && std::min(startY, endY) - arch < pageTop + space)
		{
			above = false;
			startY = firstBottom + space * 0.9f;
			endY = lastBottom + space * 0.9f;
		}
		else if (!above && std::max(startY, endY) + arch > pageBottom - space)
		{
			above = true;
			startY = firstTop - space * 0.9f;
			endY = lastTop - space * 0.9f;
		}

		const float direction = above ? -arch : arch;
		draw->AddBezierCubic(
			ImVec2(startX, startY),
			ImVec2(startX + span * 0.25f, startY + direction),
			ImVec2(endX - span * 0.25f, endY + direction),
			ImVec2(endX, endY), ink, std::max(1.0f, space * 0.10f));
	}

	// Each bracket spans exactly the three notes detected above, and sits clear of the beam.
	for (const TupletSpan& tuplet : tuplets)
	{
		const size_t members[3] = { tuplet.first, tuplet.second, tuplet.third };
		float topY = FLT_MAX;
		float bottomY = -FLT_MAX;
		for (size_t onsetIndex : members)
		{
			for (size_t glyphIndex : onsets[onsetIndex].glyphs)
			{
				topY = std::min(topY, glyphs[glyphIndex].y);
				bottomY = std::max(bottomY, glyphs[glyphIndex].y);
			}

			const int beamIndex = onsetBeam[onsetIndex];
			if (beamIndex < 0)
				continue;
			const BeamSpan& beam = beams[beamIndex];
			const float beamY = beam.beamStartY + beam.slope * (onsets[onsetIndex].x - beam.firstX);
			if (beam.stemUp)
				topY = std::min(topY, beamY);
			else
				bottomY = std::max(bottomY, beamY + space * 0.20f);
		}

		// Brackets stay outside their own staff: above the treble, below the bass. The other
		// staff's ledger lines and notes are then never crossed.
		bool above = tuplet.treble;
		float y = above ? topY - space * 2.4f : bottomY + space * 2.4f;
		if (above && y < pageTop + space * 2.0f)
		{
			above = false;
			y = bottomY + space * 2.4f;
		}
		else if (!above && y > pageBottom - space * 2.0f)
		{
			above = true;
			y = topY - space * 2.4f;
		}

		const Onset& first = onsets[tuplet.first];
		const Onset& last = onsets[tuplet.third];
		const float leftX = first.x - headRX * 0.9f;
		const float rightX = last.x + headRX * 0.9f;
		const float center = (leftX + rightX) * 0.5f;
		const float numberWidth = space * 1.15f;
		const float numberHeight = space * 1.45f;
		const float gapLeft = center - numberWidth * 0.6f;
		const float gapRight = center + numberWidth * 0.6f;
		const float hook = above ? space * 0.55f : -space * 0.55f;

		draw->AddLine(ImVec2(leftX, y), ImVec2(gapLeft, y), ink, std::max(1.0f, space * 0.08f));
		draw->AddLine(ImVec2(gapRight, y), ImVec2(rightX, y), ink, std::max(1.0f, space * 0.08f));
		draw->AddLine(ImVec2(leftX, y), ImVec2(leftX, y + hook), ink, std::max(1.0f, space * 0.08f));
		draw->AddLine(ImVec2(rightX, y), ImVec2(rightX, y + hook), ink, std::max(1.0f, space * 0.08f));
		DrawGlyphAnchored(sheetTime[3].get(), center - numberWidth * 0.5f, y - numberHeight * 0.5f,
			numberHeight, glyphTint);
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
			if (glyph.value < (uint32_t)quarterTicks * 4)
				anyStemmed = true;
		}
		if (!anyStemmed)
			continue;

		const bool stemUp = onsetStemUp(onset, onset.treble ? trebleCenter : bassCenter);
		const float stemX = onset.x + (stemUp ? headRX : -headRX);
		// Run one stem through the whole chord: start at the note opposite the stem tip and
		// finish past the outer notehead.
		const float attachY = stemUp ? bottomY : topY;
		const float stemTip = (stemUp ? topY : bottomY) + (stemUp ? -space * 3.25f : space * 3.25f);
		draw->AddLine(ImVec2(stemX, attachY), ImVec2(stemX, stemTip), ink, stemWidth);

		const int flags = beamLevel(onset.value);
		if (flags > 0)
		{
			GPUImage* flag = flags >= 4 ? (stemUp ? flagUp64.get() : flagDown64.get())
				: flags == 3 ? (stemUp ? flagUp32.get() : flagDown32.get())
				: flags == 2 ? (stemUp ? flagUp16.get() : flagDown16.get())
				: (stemUp ? flagUp.get() : flagDown.get());
			const float flagHeight = space * (flags >= 4 ? 4.64f : flags == 3 ? 3.85f : 3.28f);
			if (stemUp)
				DrawGlyphAnchored(flag, stemX, stemTip, flagHeight, glyphTint);
			else
				DrawGlyphAnchored(flag, stemX, stemTip - flagHeight, flagHeight, glyphTint);
		}
	}

	for (size_t i = 0; i < glyphs.size(); ++i)
	{
		const NoteGlyph& glyph = glyphs[i];
		const bool openHead = glyph.value >= (uint32_t)quarterTicks * 2;
		const bool wholeHead = glyph.value >= (uint32_t)quarterTicks * 4;
		const ImVec2 center(glyph.x, glyph.y);

		const int trebleBase = 4 * 7 + 2;
		const int bassBase = 2 * 7 + 4;
		const int base = glyph.treble ? trebleBase : bassBase;
		const float staffBottom = glyph.treble ? trebleBottom : bassBottom;
		const int diatonicStep = StaffStep((int)glyph.key, base, flatKey);

		// Ledger lines only on line positions, never in the space next to the staff, and only
		// between the staff and the note (a note in a space gets the line below it, not a
		// stray one on its far side).
		if (diatonicStep <= -2)
		{
			for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep + 1; step < 0; step += 2)
			{
				const float ledgerY = staffBottom - (float)step * space * 0.5f;
				draw->AddLine(ImVec2(glyph.x - headRX * 1.6f, ledgerY), ImVec2(glyph.x + headRX * 1.6f, ledgerY),
					staffInk, std::max(1.0f, space * 0.075f));
			}
		}
		else if (diatonicStep >= 10)
		{
			for (int step = (diatonicStep % 2 == 0) ? diatonicStep : diatonicStep - 1; step > 8; step -= 2)
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
			const float accidentalHeight = space * 2.9f;
			GPUImage* accidental = naturalSign ? sheetNatural.get() : (flatKey ? sheetFlat.get() : sheetSharp.get());
			const float accidentalWidth = accidental && accidental->height > 0
				? accidentalHeight * (float)accidental->width / (float)accidental->height
				: space * 1.2f;
			DrawGlyph(accidental, glyph.x - headRX - accidentalWidth * 0.5f - space * 0.2f, glyph.y,
				accidentalHeight, glyphTint);
		}

		const float noteheadHeight = space * 1.0f;
		GPUImage* notehead = wholeHead ? noteWhole.get() : (openHead ? noteHalf.get() : noteBlack.get());
		DrawGlyph(notehead, glyph.x, glyph.y, noteheadHeight, glyphTint);

		// A dot lengthens a note by half. Use the notated rhythm (inter-onset spacing), so a
		// held eighth does not turn into a dotted note just because its gate is long.
		const long dottedTolerance = std::max(2L, quarterTicks / 16);
		const bool dotted = std::abs((long)glyph.gate - quarterTicks * 3 / 2) <= dottedTolerance
			|| std::abs((long)glyph.gate - quarterTicks * 3 / 4) <= dottedTolerance;
		if (dotted)
		{
			const float dotY = glyph.y + (diatonicStep % 2 == 0 ? space * 0.5f : 0.0f);
			draw->AddCircleFilled(ImVec2(glyph.x + headRX + space * 0.62f, dotY),
				std::max(1.5f, space * 0.14f), ink, 12);
		}

		// The reference marks sounding notes with a red dot at the cursor.
		if (glyph.active)
			draw->AddEllipseFilled(center, ImVec2(space * 0.33f, space * 0.33f), activeInk, 0.0f, 12);
	}

	draw->PopClipRect();
}
