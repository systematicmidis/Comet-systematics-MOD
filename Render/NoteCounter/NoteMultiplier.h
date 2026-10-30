#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "Config/MIDIPlayerConfig.h"
#include "Utils.h"

// Display-only helpers for the "note farming" multiplier. They never modify real note data,
// the renderer or the audio - they only inflate the values shown by the note counter overlay.
namespace NoteMultiplier
{
	// displayed = (count - countPlain - melody) x multiplier + melody + countPlain
	//
	// `countPlain` is the part of `count` that came from note tracks the multiplier leaves alone:
	// those notes are added back at 1x instead of being inflated, so the melody track keeps its real
	// count while the farming tracks run away. 0 means every track is multiplied.
	inline std::string CountDigitsWith(uint64_t count, const std::string& multiplierDigits, uint64_t melody, uint64_t countPlain = 0)
	{
		if (multiplierDigits.empty() || multiplierDigits == "1")
			return std::to_string(count);

		const uint64_t applied = count > countPlain ? count - countPlain : 0;
		// The melody is the leading notes that stay realistic, and it can only be carved out of the
		// notes that are actually multiplied.
		const uint64_t melodyClamped = std::min(melody, applied);

		const std::string boosted = Utils::BigMulAddU64(multiplierDigits, applied - melodyClamped, melodyClamped);
		if (countPlain == 0)
			return boosted;

		return Utils::BigMulAddU64(boosted, 1, countPlain);
	}

	inline std::string CountDigits(const MIDIPlayerConfig* config, uint64_t count, uint64_t countPlain = 0)
	{
		if (config == nullptr || !config->multiplier.IsActive())
			return std::to_string(count);

		return CountDigitsWith(count, config->multiplier.value, config->multiplier.melodyNotes, countPlain);
	}

	// Note count as shown by the counter, e.g. "2,200,000,000,000".
	inline std::string FormatCount(const MIDIPlayerConfig* config, uint64_t count, uint64_t countPlain = 0)
	{
		return Utils::FormatWithCommas(CountDigits(config, count, countPlain));
	}

	// NPS / polyphony follow the same rule as the note count: nothing is boosted until the
	// melody has finished, because the melody notes themselves are not multiplied either.
	// Once past the melody a threshold of 0 boosts unconditionally; above 0 only values
	// larger than the threshold are boosted, so the calm parts of a song stay realistic.
	//
	// `valuePlain` is the part of `value` that came from tracks the multiplier leaves alone - the
	// TrackNoteCounter split measured for this same metric - and it stays at 1x, so an unticked
	// track never shows up multiplied in the rate rows either.
	//
	// `playedPlain` is the *total* number of passed notes that are untracked. When it covers every
	// passed note, the whole row is plain by definition, so nothing may be inflated even if the
	// per-metric `valuePlain` window happens to under-report it (which is exactly what bites right
	// after a MIDI loads or the playhead is seeked).
	inline std::string FormatBoosted(const MIDIPlayerConfig* config, uint64_t value, uint64_t notesPassed,
		uint64_t valuePlain = 0, uint64_t playedPlain = 0)
	{
		if (config == nullptr || !config->multiplier.IsActive())
			return Utils::FormatWithCommas(value);

		// Every note seen so far is untracked, so there is no multiplied note in this row at all.
		if (notesPassed > 0 && playedPlain >= notesPassed)
			return Utils::FormatWithCommas(value);

		if (notesPassed <= config->multiplier.melodyNotes)
			return Utils::FormatWithCommas(value);

		// Nothing in this row comes from a multiplied track, so there is nothing to inflate.
		const uint64_t plain = std::min(valuePlain, value);
		if (plain == value)
			return Utils::FormatWithCommas(value);

		const uint64_t threshold = config->multiplier.npsPolyBoostThreshold;
		if (threshold > 0 && value <= threshold)
			return Utils::FormatWithCommas(value);

		const std::string boosted = Utils::BigMulAddU64(config->multiplier.value, value - plain, 0);
		if (plain == 0)
			return Utils::FormatWithCommas(boosted);

		return Utils::FormatWithCommas(Utils::BigMulAddU64(boosted, 1, plain));
	}

	// Parses a decimal-digit multiplier into a double, for the graph. Beyond MAX_RATE it saturates
	// rather than turning into infinity.
	inline double MultiplierFactor(const std::string& digits)
	{
		constexpr double MAX_RATE = 1.0e300;

		double factor = 0.0;
		for (char c : digits)
		{
			if (c < '0' || c > '9')
				continue;

			factor = factor * 10.0 + (double)(c - '0');
			if (factor > MAX_RATE)
				return MAX_RATE;
		}

		return factor < 1.0 ? 1.0 : factor;
	}

	// Numeric sibling of FormatBoosted, for the graph. It applies exactly the same rules - melody,
	// threshold and the per-track plain split - but returns a number that can be plotted, so the
	// graph's lines and the counter's NPS/Polyphony rows always agree. `multiply` is the graph's own
	// switch: when off, the real rate is returned even while the counter keeps farming.
	inline double DisplayedRate(const MIDIPlayerConfig* config, uint64_t value, uint64_t notesPassed,
		uint64_t valuePlain, bool multiply, uint64_t playedPlain = 0)
	{
		if (config == nullptr || !multiply || !config->multiplier.IsActive())
			return (double)value;

		// Mirrors FormatBoosted: when every passed note is untracked the row is entirely plain.
		if (notesPassed > 0 && playedPlain >= notesPassed)
			return (double)value;

		if (notesPassed <= config->multiplier.melodyNotes)
			return (double)value;

		const uint64_t plain = std::min(valuePlain, value);
		if (plain == value)
			return (double)value;

		const uint64_t threshold = config->multiplier.npsPolyBoostThreshold;
		if (threshold > 0 && value <= threshold)
			return (double)value;

		// Only the part that came from a multiplied track is inflated; the rest is added back at 1x.
		return (double)(value - plain) * MultiplierFactor(config->multiplier.value) + (double)plain;
	}
}
