#include "MIDISequence.h"
#include "Sequence/SequenceFuncs.h"
#include <iostream>
#include <algorithm>

void MIDISequence::StartMergedNotesPreparation()
{
	if (!mergedNotes.empty())
	{
		mergeState.store(2, std::memory_order_release);
		mergeProgress.store(1.0f, std::memory_order_relaxed);
		return;
	}

	if (trackNotes.empty())
	{
		mergeState.store(2, std::memory_order_release);
		mergeProgress.store(1.0f, std::memory_order_relaxed);
		return;
	}

	int expected = 0;
	if (!mergeState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
		return;

	mergeProgress.store(0.02f, std::memory_order_relaxed);
	std::cout << "Building per-key render store for the selected renderer..." << std::endl;

	try
	{
	std::lock_guard<std::mutex> futureLock(mergeFutureMutex);
	mergeFuture = std::async(std::launch::async, [this]()
		{
			try
			{
				mergeProgress.store(0.05f, std::memory_order_relaxed);
				std::vector<NoteSequence> tracks = trackNotes;
				mergeProgress.store(0.12f, std::memory_order_relaxed);
				size_t deduplicated = 0;
				auto merged = SequenceFuncs::WindowedMergeAndDistribute(
					std::move(tracks), (uint32_t)length, &deduplicated);
				mergeProgress.store(0.85f, std::memory_order_relaxed);

				std::array<std::vector<NoteBlock>, MIDI_KEYS> blocks{};
				for (size_t key = 0; key < MIDI_KEYS; key++)
				{
					const NoteSequence& notes = merged[key];
					std::vector<NoteBlock> keyBlocks;
					keyBlocks.reserve((notes.Size() + NOTE_BLOCK_SIZE - 1) / NOTE_BLOCK_SIZE);

					uint32_t blockMin = (uint32_t)(-1);
					uint32_t blockMax = 0;
					for (size_t i = 0; i < notes.Size(); ++i)
					{
						blockMin = std::min(blockMin, notes.tick[i]);
						blockMax = std::max(blockMax, notes.tick[i] + notes.gate[i]);
						if ((i + 1) % NOTE_BLOCK_SIZE == 0)
						{
							keyBlocks.emplace_back(blockMin, blockMax);
							blockMin = (uint32_t)(-1);
							blockMax = 0;
						}
					}
					if (notes.Size() % NOTE_BLOCK_SIZE != 0)
						keyBlocks.emplace_back(blockMin, blockMax);

					blocks[key] = std::move(keyBlocks);
				}

				std::lock_guard<std::mutex> lock(mergeMutex);
				mergedNotes = std::move(merged);
				noteBlocks = std::move(blocks);
				mergeProgress.store(1.0f, std::memory_order_relaxed);
				mergeState.store(2, std::memory_order_release);
			}
			catch (const std::exception& e)
			{
				std::cerr << "Could not build per-key render store: " << e.what() << std::endl;
				mergeFailed.store(true, std::memory_order_release);
				mergeProgress.store(1.0f, std::memory_order_relaxed);
				mergeState.store(2, std::memory_order_release);
			}
			catch (...)
			{
				std::cerr << "Could not build per-key render store: out of memory or unknown error" << std::endl;
				mergeFailed.store(true, std::memory_order_release);
				mergeProgress.store(1.0f, std::memory_order_relaxed);
				mergeState.store(2, std::memory_order_release);
			}
		});
	}
	catch (const std::exception& e)
	{
		std::cerr << "Could not start per-key preparation: " << e.what() << std::endl;
		mergeFailed.store(true, std::memory_order_release);
		mergeProgress.store(1.0f, std::memory_order_relaxed);
		mergeState.store(2, std::memory_order_release);
	}
}

void MIDISequence::WaitForMergedNotes()
{
	std::future<void> pending;
	{
		std::lock_guard<std::mutex> lock(mergeFutureMutex);
		if (mergeFuture.valid()) pending = std::move(mergeFuture);
	}
	if (pending.valid()) pending.wait();
}

void MIDISequence::BuildSparseSeekIndex()
{
	if (trackNotes.empty()) return;
	sparseIndex.Build(trackNotes);
	std::cout << "  Seek index: " << sparseIndex.TrackCount() << " track(s), "
		<< sparseIndex.PageCount() << " sparse page(s)" << std::endl;
}

void MIDISequence::EnsureMergedNotes()
{
	if (!mergedNotes.empty()) return;

	StartMergedNotesPreparation();
	WaitForMergedNotes();
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