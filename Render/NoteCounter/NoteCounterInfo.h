#pragma once

#include <cmath>
#include <cstdint>
#include <deque>

struct NoteHistoryPoint
{
	double timeSeconds;
	uint64_t totalNotes;
};

template <typename T>
struct NoteCounterField
{
	T value = T{};
	bool shown = true;

	NoteCounterField() = default;
	NoteCounterField(T value) : value(value) {}
	void operator=(T value) { this->value = value; }
};

struct NoteCounterInfo
{
	NoteCounterField<uint64_t> notesPassed{ 0 };
	NoteCounterField<uint64_t> polyphony{ 0 };
	NoteCounterField<double> timeSeconds{ 0 };
	NoteCounterField<uint64_t> notesPerSecond{ 0 };
	NoteCounterField<double> bpm{ 120 };
	NoteCounterField<uint64_t> tick{ 0 };
	NoteCounterField<uint16_t> ppq{ 960 };

	// The loader may keep one copy of repeated notes/layers while counting every copy for the
	// user. This factor maps the notes the renderer walks back to the file's declared total, so
	// the counter never shows a deduplicated number.
	double noteScale = 1.0;

	uint64_t Scale(uint64_t value) const
	{
		if (noteScale <= 1.0) return value;
		const long double scaled = (long double)value * (long double)noteScale;
		return scaled >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)std::llround(scaled);
	}

	std::deque<NoteHistoryPoint> npsHistory;
	void ResetCounter();
};