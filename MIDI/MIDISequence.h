#pragma once

#include <array>
#include <vector>
#include "MIDITrack.h"
#include "Events/TempoEvent.h"
#include "Events/NoteEvent.h"
#include "Events/TimeSignatureEvent.h"
#include <fstream>
#include <memory>
#include "MIDIDefs.h"
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
	// Extra multiplier for a proven repeated-cycle archive. Aura stacking uses this (capped)
	// so a compressed file looks as stacked as its decoded cycles would.
	float compressedStackScale = 1.0f;
	bool packed = false;
	std::array<std::vector<NoteBlock>, MIDI_KEYS> noteBlocks{};
	std::vector<MIDIMessageEvent> mergedEvents{};
	std::shared_ptr<TempoMap> tempoMap = nullptr;
	size_t noteTrackCount;
	long long notes = 0;
	long length = 0;
	uint32_t longestNoteLength = 0;
	bool timeBased = false;

	MIDISequence() : MIDISequence("Unnamed") {}
	MIDISequence(const char* name)
		: name(name), tracks({}), tempos({}), timeSignatures({}) {}
	~MIDISequence()
	{
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