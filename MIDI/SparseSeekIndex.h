#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "Events/NoteEvent.h"

// A small, immutable index over the packed track store. It does not duplicate notes: each track
// keeps its first/last tick, one tick per 4,096-note page, and the largest end tick per span of
// 512 notes (plus a running prefix maximum). That gives a seek a coarse landing point, lets the
// renderer skip tracks that cannot overlap the visible window, and finds the few notes whose tails
// cross the back-cull line without walking the track from its first note.
class SparseSeekIndex
{
public:
	static constexpr size_t PAGE_NOTES = 4096;
	static constexpr size_t SPAN_NOTES = 512;

	struct TrackInfo
	{
		uint32_t firstTick = 0;
		uint32_t lastTick = 0;
		size_t noteCount = 0;
		std::vector<uint32_t> pageTicks{};
		// Largest (tick + gate) inside each span, and the running maximum up to that span.
		std::vector<uint32_t> spanMaxEnd{};
		std::vector<uint32_t> spanPrefixMax{};
	};

	void Build(const std::vector<NoteSequence>& tracks)
	{
		info.clear();
		info.reserve(tracks.size());

		for (const NoteSequence& notes : tracks)
		{
			TrackInfo track;
			track.noteCount = notes.Size();
			if (track.noteCount == 0)
			{
				info.push_back(std::move(track));
				continue;
			}

			track.firstTick = notes.tick.front();
			track.lastTick = notes.tick.back();
			track.pageTicks.reserve((track.noteCount + PAGE_NOTES - 1) / PAGE_NOTES);
			track.spanMaxEnd.reserve((track.noteCount + SPAN_NOTES - 1) / SPAN_NOTES);

			uint32_t longestGate = 0;
			uint32_t spanMax = 0;
			for (size_t i = 0; i < track.noteCount; ++i)
			{
				const uint32_t gate = notes.gate[i];
				longestGate = std::max(longestGate, gate);
				spanMax = std::max(spanMax, notes.tick[i] + gate);

				if ((i & (SPAN_NOTES - 1)) == SPAN_NOTES - 1)
				{
					track.spanMaxEnd.push_back(spanMax);
					spanMax = 0;
				}
				if ((i & (PAGE_NOTES - 1)) == 0)
					track.pageTicks.push_back(notes.tick[i]);
			}
			if (track.noteCount % SPAN_NOTES != 0)
				track.spanMaxEnd.push_back(spanMax);
			track.lastTick += longestGate;

			track.spanPrefixMax.resize(track.spanMaxEnd.size());
			uint32_t running = 0;
			for (size_t i = 0; i < track.spanMaxEnd.size(); ++i)
			{
				running = std::max(running, track.spanMaxEnd[i]);
				track.spanPrefixMax[i] = running;
			}

			info.push_back(std::move(track));
		}
	}

	bool Empty() const { return info.empty(); }
	size_t TrackCount() const { return info.size(); }
	size_t PageCount() const
	{
		size_t count = 0;
		for (const TrackInfo& track : info) count += track.pageTicks.size();
		return count;
	}

	bool Overlaps(size_t track, uint32_t startTick, uint32_t endTick) const
	{
		if (track >= info.size()) return false;
		const TrackInfo& t = info[track];
		return t.noteCount != 0 && t.lastTick >= startTick && t.firstTick <= endTick;
	}

	// First note whose start tick is >= tick. The page search finds a 4,096-note block, then the
	// exact search only walks that block. On giant single tracks this is materially cheaper than
	// a binary search over tens of millions of notes.
	size_t LowerBound(size_t track, uint32_t tick, const std::vector<NoteSequence>& tracks) const
	{
		if (track >= info.size() || track >= tracks.size()) return 0;
		const NoteSequence& notes = tracks[track];
		const TrackInfo& t = info[track];
		if (notes.Empty()) return 0;

		size_t page = 0;
		if (!t.pageTicks.empty())
		{
			auto it = std::upper_bound(t.pageTicks.begin(), t.pageTicks.end(), tick);
			page = it == t.pageTicks.begin() ? 0 : (size_t)(it - t.pageTicks.begin() - 1);
		}

		const size_t begin = std::min(notes.Size(), page * PAGE_NOTES);
		const size_t end = std::min(notes.Size(), begin + PAGE_NOTES);
		auto it = std::lower_bound(notes.tick.begin() + begin, notes.tick.begin() + end, tick);
		return (size_t)(it - notes.tick.begin());
	}

	// First span that holds a note ending after `tick`. Every note in an earlier span ends at or
	// before `tick`, so this is exactly where tails crossing that line can begin.
	size_t FirstSpanEndingAfter(size_t track, uint32_t tick) const
	{
		if (track >= info.size()) return 0;
		const TrackInfo& t = info[track];
		if (t.spanPrefixMax.empty()) return 0;

		auto it = std::upper_bound(t.spanPrefixMax.begin(), t.spanPrefixMax.end(), tick);
		return (size_t)(it - t.spanPrefixMax.begin());
	}

	const TrackInfo* Info(size_t track) const
	{
		return track < info.size() ? &info[track] : nullptr;
	}

	uint32_t FirstTick(size_t track) const
	{
		return track < info.size() ? info[track].firstTick : 0;
	}

	uint32_t LastTick(size_t track) const
	{
		return track < info.size() ? info[track].lastTick : 0;
	}

private:
	std::vector<TrackInfo> info;
};
