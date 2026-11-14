#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "MIDI/Sequence/SequenceFuncs.h"

// The windowed merge replaced a heap that combined every track at once. Both are kept, so this
// checks the replacement really produces the same per-key note order the old one did, both when
// it scans for the maximum tick and when the caller supplies it the way the loader does.

namespace
{
	int failures = 0;

	void Check(const char* name, bool ok, const std::string& detail)
	{
		std::printf("%s %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
		if (!ok) failures++;
	}

	bool MatchesHeap(const std::vector<NoteSequence>& heap, const std::vector<NoteSequence>& windowed,
		int trial, std::string& where)
	{
		for (int key = 0; key < MIDI_KEYS; key++)
		{
			if (heap[key].Size() != windowed[key].Size())
			{
				where = "trial " + std::to_string(trial) + ", key " + std::to_string(key) + ": "
					+ std::to_string(heap[key].Size()) + " notes vs " + std::to_string(windowed[key].Size());
				return false;
			}

			for (size_t i = 0; i < heap[key].Size(); i++)
			{
				if (heap[key].tick[i] != windowed[key].tick[i]
					|| heap[key].gate[i] != windowed[key].gate[i]
					|| heap[key].packed[i] != windowed[key].packed[i])
				{
					where = "trial " + std::to_string(trial) + ", key " + std::to_string(key)
						+ ", note " + std::to_string(i);
					return false;
				}
			}
		}

		return true;
	}
}

int main()
{
	std::mt19937 rng(12345);

	bool same = true;
	std::string where = "20 random track sets";

	bool sameHinted = true;
	std::string whereHinted = "20 random track sets";

	for (int trial = 0; trial < 20 && (same || sameHinted); trial++)
	{
		const int trackCount = 1 + (int)(rng() % 12);
		std::vector<NoteSequence> original;
		uint32_t maxTick = 0;

		for (int t = 0; t < trackCount; t++)
		{
			NoteSequence track;
			uint32_t tick = 0;
			const int notes = (int)(rng() % 200);

			for (int i = 0; i < notes; i++)
			{
				tick += (uint32_t)(rng() % 40);
				track.Emplace((uint16_t)t, (uint8_t)(rng() % 16), tick, (uint8_t)(rng() % 128),
					(uint32_t)(rng() % 500), (uint8_t)(1 + rng() % 127));
			}

			if (tick > maxTick) maxTick = tick;
			original.push_back(std::move(track));
		}

		std::vector<NoteSequence> copy = original;
		std::vector<NoteSequence> hintedCopy = original;
		std::vector<NoteSequence> heap = SequenceFuncs::MergeAndDistribute(std::move(original));

		if (same)
		{
			std::vector<NoteSequence> windowed =
				SequenceFuncs::WindowedMergeAndDistribute(std::move(copy));
			same = MatchesHeap(heap, windowed, trial, where);
		}

		if (sameHinted)
		{
			// The loader hands over a length that bounds every tick, which skips the merge's own
			// scan and takes a different window size; the result must be identical either way.
			std::vector<NoteSequence> windowed = SequenceFuncs::WindowedMergeAndDistribute(
				std::move(hintedCopy), maxTick + 16);
			sameHinted = MatchesHeap(heap, windowed, trial, whereHinted);
		}
	}

	Check("windowed merge matches the heap merge", same, where);
	Check("windowed merge matches the heap merge with a max-tick hint", sameHinted, whereHinted);

	std::printf("\n%s\n", failures == 0 ? "ALL PASS" : (std::to_string(failures) + " FAILURE(S)").c_str());
	return failures == 0 ? 0 : 1;
}
