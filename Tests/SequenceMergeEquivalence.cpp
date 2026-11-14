#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "MIDI/Sequence/SequenceFuncs.h"

// The windowed merge replaced a heap that combined every track at once. Both are kept, so this
// checks the replacement really produces the same per-key note order the old one did.

namespace
{
	int failures = 0;

	void Check(const char* name, bool ok, const std::string& detail)
	{
		std::printf("%s %-46s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
		if (!ok) failures++;
	}
}

int main()
{
	std::mt19937 rng(12345);
	bool same = true;
	std::string where = "20 random track sets";

	for (int trial = 0; trial < 20 && same; trial++)
	{
		const int trackCount = 1 + (int)(rng() % 12);
		std::vector<NoteSequence> original;

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

			original.push_back(std::move(track));
		}

		std::vector<NoteSequence> copy = original;
		std::vector<NoteSequence> heap = SequenceFuncs::MergeAndDistribute(std::move(original));
		std::vector<NoteSequence> windowed = SequenceFuncs::WindowedMergeAndDistribute(std::move(copy));

		for (int key = 0; key < MIDI_KEYS && same; key++)
		{
			if (heap[key].Size() != windowed[key].Size())
			{
				same = false;
				where = "trial " + std::to_string(trial) + ", key " + std::to_string(key) + ": "
					+ std::to_string(heap[key].Size()) + " notes vs " + std::to_string(windowed[key].Size());
				break;
			}

			for (size_t i = 0; i < heap[key].Size(); i++)
			{
				if (heap[key].tick[i] != windowed[key].tick[i]
					|| heap[key].gate[i] != windowed[key].gate[i]
					|| heap[key].packed[i] != windowed[key].packed[i])
				{
					same = false;
					where = "trial " + std::to_string(trial) + ", key " + std::to_string(key)
						+ ", note " + std::to_string(i);
					break;
				}
			}
		}
	}

	Check("windowed merge matches the heap merge", same, where);

	std::printf("\n%s\n", failures == 0 ? "ALL PASS" : (std::to_string(failures) + " FAILURE(S)").c_str());
	return failures == 0 ? 0 : 1;
}
