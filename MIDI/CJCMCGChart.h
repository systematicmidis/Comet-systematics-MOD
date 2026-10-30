#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Events/TempoEvent.h"

// A .CJCMCG file, parsed as the note-density chart it actually is.
//
// The format is big-endian:
//   <PPQ, u32> <BPM count, u32> [<tick, u32><usec per quarter, u24>] * count
//   <Lyrics count, u32> [<tick, u32><size, u32><bytes>] * count
//   <Note count, VLQ> <Tick count, u32> [<notesOn, VLQ><notesOff, VLQ>] * Tick count
//
// The note data is one entry per tick: how many notes start at that tick and how many end. It is
// NOT delta-encoded note timings - the on/off values are note counts, and they sum to the declared
// note count (verified against real files). The totals run into the trillions, so the chart is kept
// as cumulative counts instead of individual notes.
struct CJCMCGChart
{
	uint32_t ppq = 384;
	std::vector<TempoEvent> tempos;

	// cumOn[i] = notes that have started before tick i, for i in [0, tickCount].
	// cumOff[i] = notes that have ended before tick i. Both have tickCount + 1 entries.
	std::vector<uint64_t> cumOn;
	std::vector<uint64_t> cumOff;

	uint32_t tickCount = 0;
	uint64_t totalNotes = 0;   // declared note count (sum of the on-counts)
	size_t listedEntries = 0;
	size_t lyricCount = 0;

	// Notes started / ended / sounding at a fractional tick.
	uint64_t NotesStartedAt(double tick) const;
	uint64_t NotesEndedAt(double tick) const;
	uint64_t PolyphonyAt(double tick) const;

	double TickToSeconds(double tick) const;
	double SecondsToTick(double seconds) const;
	double LengthSeconds() const { return TickToSeconds((double)tickCount); }

	static constexpr const char* EXTENSION = "cjcmcg";
	static bool IsCJCMCGFile(const std::string& path);

	// Parses an in-memory image. Throws std::runtime_error if it is malformed or truncated.
	static std::shared_ptr<CJCMCGChart> Parse(const uint8_t* data, size_t size);
	// Reads and parses a file. Throws on failure.
	static std::shared_ptr<CJCMCGChart> LoadFile(const std::string& path);
};
