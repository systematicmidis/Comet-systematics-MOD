#include "MIDISequence.h"
#include "Sequence/SequenceFuncs.h"
#include <iostream>
#include <algorithm>

void MIDISequence::EnsureMergedNotes()
{
	if (!mergedNotes.empty() || trackNotes.empty()) return;

	std::cout << "Building per-key render store for the selected renderer..." << std::endl;

	std::vector<NoteSequence> tracks = trackNotes;
	size_t deduplicated = 0;
	mergedNotes = SequenceFuncs::WindowedMergeAndDistribute(
		std::move(tracks), (uint32_t)length, &deduplicated);

	for (size_t key = 0; key < MIDI_KEYS; key++)
	{
		const NoteSequence& notes = mergedNotes[key];
		std::vector<NoteBlock> blocks;
		blocks.reserve((notes.Size() + NOTE_BLOCK_SIZE - 1) / NOTE_BLOCK_SIZE);

		uint32_t blockMin = (uint32_t)(-1);
		uint32_t blockMax = 0;
		for (size_t i = 0; i < notes.Size(); ++i)
		{
			blockMin = std::min(blockMin, notes.tick[i]);
			blockMax = std::max(blockMax, notes.tick[i] + notes.gate[i]);
			if ((i + 1) % NOTE_BLOCK_SIZE == 0)
			{
				blocks.emplace_back(blockMin, blockMax);
				blockMin = (uint32_t)(-1);
				blockMax = 0;
			}
		}
		if (notes.Size() % NOTE_BLOCK_SIZE != 0)
			blocks.emplace_back(blockMin, blockMax);

		noteBlocks[key] = std::move(blocks);
	}

	packed = false;
}

double MIDISequence::CalcLengthMilliseconds()
{
	double msTotal = 0;
	long tick = 0L;
	double bpm = 120;
	for (int i = 0; i < tempos.size(); i++)
	{
		TempoEvent* t = &(tempos[i]);
		msTotal += 60000 / bpm * (t->tick - tick) / resolution;
		tick = t->tick;
		bpm = t->bpm;
	}
	return msTotal + 60000.0 / bpm * (length - tick) / resolution;
}