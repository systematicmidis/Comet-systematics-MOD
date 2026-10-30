#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>

#include "MIDI/CJCMCGChart.h"

// Turns a .CJCMCG chart into the numbers the note counter and graph show: notes started so far,
// notes currently sounding, and notes started in the last second.
//
// The chart stores cumulative per-tick counts, so this is O(1) per frame - the trillions of notes
// are never materialised. The playhead is in seconds and is mapped onto the chart's own timeline.
class CounterChartScan
{
public:
	void Update(const CJCMCGChart* chart, double playbackSeconds)
	{
		cachedChart = chart;

		if (chart == nullptr)
		{
			notesPassed = 0;
			polyphony = 0;
			notesPerSecond = 0;
			history.clear();
			return;
		}

		// A seek backwards would draw a line across the graph, so drop the history there.
		if (!history.empty() && playbackSeconds < history.back().time)
			history.clear();

		const double tick = chart->SecondsToTick(playbackSeconds);
		notesPassed = chart->NotesStartedAt(tick);
		polyphony = chart->PolyphonyAt(tick);

		history.push_back({ playbackSeconds, notesPassed });
		while (!history.empty() && (playbackSeconds - history.front().time) > 1.0)
			history.pop_front();

		const uint64_t baseline = history.empty() ? notesPassed : history.front().notes;
		notesPerSecond = notesPassed >= baseline ? notesPassed - baseline : 0;
	}

	uint64_t NotesPassed() const { return notesPassed; }
	uint64_t Polyphony() const { return polyphony; }
	uint64_t NotesPerSecond() const { return notesPerSecond; }

private:
	struct Sample
	{
		double time;
		uint64_t notes;
	};

	const CJCMCGChart* cachedChart = nullptr;
	uint64_t notesPassed = 0;
	uint64_t polyphony = 0;
	uint64_t notesPerSecond = 0;
	std::deque<Sample> history{};
};
