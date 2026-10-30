#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "MIDI/MIDISequence.h"

// Half of the per-track multiplier: the renderers report the totals (notes passed, NPS, polyphony)
// and this reports how much of each belongs to tracks the multiplier was told to leave alone. The
// overlay then boosts only the difference, so an unticked melody stays realistic in every row.
//
// It walks the same per-key note lists the renderers walk, but keeps a cursor per key, so a note is
// examined once as the playhead passes it. Backwards jumps (a seek) and changes of MIDI or track
// selection cost one pass over the notes already played, and nothing else is per-frame work.
class TrackNoteCounter
{
public:
	// `threshold` is in the same units as NoteSequence::tick: ticks normally, or
	// seconds * TIME_BASED_MULTIPLIER for a sequence that was loaded time-based. `playbackSeconds`
	// is the clock the renderers use for their own NPS window. Set `wantPolyphony` while the
	// polyphony row is on screen, since counting sounding notes costs more than the rest.
	void Update(const MIDISequence* sequence, double playbackSeconds, double threshold,
		const std::vector<uint16_t>& unmultiplied, bool wantPolyphony)
	{
		const bool reselecting = sequence != cachedSequence || unmultiplied != cachedUnmultiplied;
		cachedSequence = sequence;
		cachedUnmultiplied = unmultiplied;

		if (reselecting || threshold < lastThreshold || cursors.size() != KeyCount(sequence))
		{
			// Different MIDI or track selection, or the playhead moved backwards, so rewind and let
			// the loops below walk forward to the current position again.
			cursors.assign(KeyCount(sequence), 0);
			soundingBlocks.assign(KeyCount(sequence), 0);
			playedPlain = 0;
			soundingPlain = 0;
			lastThreshold = 0.0;
			plainWindowComplete = false;
			haveWindowStart = false;
			history.clear();
		}

		lastThreshold = threshold;

		if (sequence == nullptr || unmultiplied.empty())
		{
			// Every track is multiplied, so there is no plain part to report.
			playedPlain = 0;
			soundingPlain = 0;
			plainNotesPerSecond = 0;
			plainWindowComplete = false;
			haveWindowStart = false;
			history.clear();
			return;
		}

		const std::vector<NoteSequence>& byKey = sequence->mergedNotes;
		if (cursors.size() != byKey.size())
			return;

		for (std::size_t key = 0; key < byKey.size(); ++key)
		{
			const NoteSequence& notes = byKey[key];
			const std::size_t count = notes.Size();
			std::size_t& cursor = cursors[key];

			// Each key's notes are sorted by tick, so this stops as soon as it reaches the future.
			while (cursor < count && (double)notes.tick[cursor] <= threshold)
			{
				if (!IsMultiplied(notes.GetTrack(cursor), unmultiplied))
					++playedPlain;

				++cursor;
			}
		}

		soundingPlain = wantPolyphony ? CountSounding(*sequence, threshold, unmultiplied) : 0;

		// The same one-second window the renderers use for NPS, over the unticked notes only.
		if (!history.empty() && playbackSeconds < history.back().time)
			history.clear();

		history.push_back({ playbackSeconds, playedPlain });
		while (!history.empty() && (playbackSeconds - history.front().time) > 1.0)
			history.pop_front();

		const uint64_t baseline = history.empty() ? playedPlain : history.front().plain;
		plainNotesPerSecond = playedPlain >= baseline ? playedPlain - baseline : 0;

		// The window only lines up with the renderers' own NPS baseline once it has actually been
		// sampled for a full second; see PlainNotesPerSecond(). This is measured from when sampling
		// started rather than from the oldest kept sample, so the pop boundary (which sits exactly at
		// one second) can't flip the answer on a floating-point comparison.
		if (!haveWindowStart)
		{
			windowStart = playbackSeconds;
			haveWindowStart = true;
		}

		plainWindowComplete = (playbackSeconds - windowStart) >= 1.0;
	}

	// Played notes from tracks the multiplier leaves alone.
	uint64_t PlayedPlain() const { return playedPlain; }

	// Those notes started within the last second, matching how the renderers measure NPS.
	//
	// Until the history spans a full second its oldest sample isn't a real "one second ago" point,
	// while the renderers' own NPS baseline is the start of playback. Subtracting it would then
	// under-report the plain part, and the multiplier would inflate a rate that was entirely plain.
	// During that first second the whole played-plain count is reported instead: it is measured from
	// the same beginning, so it can only under-state the multiplied part, never over-state it.
	uint64_t PlainNotesPerSecond() const { return plainWindowComplete ? plainNotesPerSecond : playedPlain; }

	// Those notes that are sounding right now, matching how the renderers measure polyphony.
	uint64_t PolyphonyPlain() const { return soundingPlain; }

private:
	static std::size_t KeyCount(const MIDISequence* sequence)
	{
		return sequence != nullptr ? sequence->mergedNotes.size() : 0;
	}

	static bool IsMultiplied(uint16_t track, const std::vector<uint16_t>& unmultiplied)
	{
		return std::find(unmultiplied.begin(), unmultiplied.end(), track) == unmultiplied.end();
	}

	// Sounding notes are the ones that started but haven't finished yet. Blocks whose last note has
	// already ended are skipped in bulk, exactly like the renderers' own polyphony pass, so the
	// per-note work is limited to the blocks that are still open.
	uint64_t CountSounding(const MIDISequence& sequence, double threshold, const std::vector<uint16_t>& unmultiplied)
	{
		uint64_t sounding = 0;

		for (std::size_t key = 0; key < sequence.mergedNotes.size() && key < soundingBlocks.size(); ++key)
		{
			const std::vector<NoteBlock>& blocks = sequence.noteBlocks[key];
			if (blocks.empty())
				continue;

			std::size_t& block = soundingBlocks[key];
			while (block < blocks.size() && (double)blocks[block].maxBound <= threshold)
				++block;

			const NoteSequence& notes = sequence.mergedNotes[key];
			const std::size_t cursor = std::min(cursors[key], notes.Size());

			for (std::size_t i = block * NOTE_BLOCK_SIZE; i < cursor; ++i)
			{
				if ((double)(notes.tick[i] + notes.gate[i]) <= threshold)
					continue;

				if (!IsMultiplied(notes.GetTrack(i), unmultiplied))
					++sounding;
			}
		}

		return sounding;
	}

	struct PlainSample
	{
		double time;
		uint64_t plain;
	};

	const MIDISequence* cachedSequence = nullptr;
	std::vector<uint16_t> cachedUnmultiplied{};
	std::vector<std::size_t> cursors{};
	std::vector<std::size_t> soundingBlocks{};
	uint64_t playedPlain = 0;
	uint64_t soundingPlain = 0;
	uint64_t plainNotesPerSecond = 0;
	bool plainWindowComplete = false;
	// Playback time of the first sample after a reset, used to tell a real one-second window from a
	// history that has only just started.
	double windowStart = 0.0;
	bool haveWindowStart = false;
	double lastThreshold = 0.0;
	std::deque<PlainSample> history{};
};
