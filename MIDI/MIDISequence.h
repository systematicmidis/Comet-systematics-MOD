#pragma once

#include <array>
#include <vector>
#include "MIDITrack.h"
#include "Events/TempoEvent.h"
#include "Events/NoteEvent.h"
#include "Events/TimeSignatureEvent.h"
#include <fstream>
#include <memory>
#include <future>
#include <mutex>
#include <atomic>
#include "MIDIDefs.h"
#include "SparseSeekIndex.h"
#define NOTE_BLOCK_SIZE 1024

class TempoMap;

class MIDISequence
{
public:
	const char* name;
	uint16_t resolution;
	uint16_t trackCount;
	std::vector<MIDITrack> tracks{};
	// Name of each note track (0-based, matching NOTE_TRACK_BIT_IDX in the packed note data).
	// Empty when the file didn't name the track.
	std::vector<std::string> noteTrackNames{};
	std::vector<TempoEvent> tempos{};
	std::vector<TimeSignatureEvent> timeSignatures{};
	std::vector<NoteSequence> mergedNotes{};
	// Large files keep the parse output as tick-ordered tracks instead of building the 128
	// per-key render arrays. MIDITrail and the audio event builders can walk these directly;
	// renderers that need per-key lists call EnsureMergedNotes() when they are selected.
	std::vector<NoteSequence> trackNotes{};
	// Number of archive copies represented by each stored track. Packed loading stores one
	// copy of a repeated track, so this lets the aura reflect the stack that was collapsed.
	std::vector<uint16_t> trackCopies{};
	// Repeated note-ons collapsed inside a single huge track (packed loading stores one copy).
	// Kept capped because the aura only needs to know that the stack is heavy.
	std::vector<uint16_t> trackNoteWeights{};
	// The note's baked track index is the UI/note-track index, which can skip entries when
	// tracks have no notes or are repeated. This maps it to the corresponding stored track.
	std::vector<int> noteTrackStoredIndex{};
	// Extra multiplier for a proven repeated-cycle archive. Aura stacking uses this (capped)
	// so a compressed file looks as stacked as its decoded cycles would.
	float compressedStackScale = 1.0f;
	bool packed = false;
	std::array<std::vector<NoteBlock>, MIDI_KEYS> noteBlocks{};

	// Packed files normally never build mergedNotes. Selecting a renderer that needs per-key
	// lists starts this on a worker so the window keeps rendering instead of freezing for the
	// duration of the conversion.
	std::atomic<int> mergeState{ 0 }; // 0 = absent, 1 = preparing, 2 = ready
	std::atomic<float> mergeProgress{ 0.0f };
	std::atomic_bool mergeFailed{ false };

	void StartMergedNotesPreparation();
	void WaitForMergedNotes();
	bool IsMergedNotesReady() const { return mergeState.load(std::memory_order_acquire) == 2; }
	bool IsMergedNotesPreparing() const { return mergeState.load(std::memory_order_acquire) == 1; }
	float GetMergedNotesProgress() const { return mergeProgress.load(std::memory_order_relaxed); }

	// Built from packed trackNotes and used by the packed renderer/audio seek paths. It is
	// immutable once built, so multiple render/worker threads can read it safely.
	void BuildSparseSeekIndex();
	const SparseSeekIndex& GetSparseSeekIndex() const { return sparseIndex; }
	std::vector<MIDIMessageEvent> mergedEvents{};
	std::shared_ptr<TempoMap> tempoMap = nullptr;
	size_t noteTrackCount;
	long long notes = 0;
	long length = 0;
	uint32_t longestNoteLength = 0;
	bool timeBased = false;

private:
	std::mutex mergeMutex;
	std::mutex mergeFutureMutex;
	std::future<void> mergeFuture;
	SparseSeekIndex sparseIndex;

public:
	MIDISequence() : MIDISequence("Unnamed") {}
	MIDISequence(const char* name)
		: name(name), tracks({}), tempos({}), timeSignatures({}) {}
	~MIDISequence()
	{
		// A renderer switch may still be converting packed tracks. Do not destroy the vector it
		// is reading from.
		WaitForMergedNotes();
		tracks.clear();
		tempos.clear();
		mergedNotes.clear();
		timeSignatures.clear();
		if (tempoMap) tempoMap.reset();
	}

	TempoMap* GetTempoMap()
	{
		return tempoMap.get();
	}
	
	long long GetNotes()
	{
		return this->notes;
	}
	long GetLength()
	{
		return this->length;
	}

	void SetLength(long length)
	{
		this->length = length;
	}

	double CalcLengthMilliseconds();

	// Lazily materialize the legacy per-key store (and its note-block index) when a renderer
	// that does not understand packed tracks is selected. Normal large-file loading never
	// calls this path.
	void EnsureMergedNotes();

	static void Read(std::ifstream* is, size_t size, uint8_t* buf)
	{
		is->read(reinterpret_cast<char*>(buf), size);
	}
	static long ReadVariableLengthValue(std::ifstream* is)
	{
		uint8_t b;
		long value = 0L;
		do
		{
			is->read(reinterpret_cast<char*>(&b), 1);
			value = (value << 7L) | (b & 0x7F);
		} while ((b & 0x80) != 0);
		return value;
	}
	static uint32_t ToInt(uint8_t* bytes)
	{
		uint32_t res = 0;
		for (int i = 0; i < 4; i++)
		{
			res = (res << 8) | static_cast<uint32_t>(bytes[i]);
		}
		return res;
	}
	static uint64_t ToLong(uint8_t* bytes)
	{
		uint64_t res = 0;
		for (int i = 0; i < 8; i++)
		{
			res = (res << 8) | static_cast<uint64_t>(bytes[i]);
		}
		return res;
	}
};