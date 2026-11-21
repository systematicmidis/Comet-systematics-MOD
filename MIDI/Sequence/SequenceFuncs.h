#pragma once

#include "../Events/MIDIEvent.h"
#include "../Events/NoteEvent.h"
#include "MIDI/TempoMap.h"
#include "MIDI/MIDIDefs.h"
#include <vector>
#include <algorithm>
#include <iterator>
#include <cmath>

class SequenceFuncs
{
public:
    template <typename T>
    static std::vector<T> MergeSequences(
        std::vector<T>&& seq1,
        std::vector<T>&& seq2)
    {
        static_assert(std::is_base_of_v<MIDIEvent, T>, "T must derive from MIDIEvent.");

        if (seq1.empty()) return std::move(seq2);
        if (seq2.empty()) return std::move(seq1);

        std::vector<T> result;
        result.reserve(seq1.size() + seq2.size());

        T* s1ptr = seq1.data();
        T* s1end = s1ptr + seq1.size();
        T* s2ptr = seq2.data();
        T* s2end = s2ptr + seq2.size();

        while (s1ptr != s1end && s2ptr != s2end)
        {
            if (s1ptr->tick <= s2ptr->tick)
                result.emplace_back(std::move(*s1ptr++));
            else
                result.emplace_back(std::move(*s2ptr++));
        }

        if (s1ptr != s1end)
            result.insert(result.end(), std::make_move_iterator(s1ptr), std::make_move_iterator(s1end));
        if (s2ptr != s2end)
            result.insert(result.end(), std::make_move_iterator(s2ptr), std::make_move_iterator(s2end));

        return result;
    }

    template <typename T>
    static std::vector<T> FlattenSequence(std::vector<std::vector<T>>&& tracks)
    {
        static_assert(std::is_base_of_v<MIDIEvent, T>, "T must derive from MIDIEvent.");

        if (tracks.empty()) return {};
        if (tracks.size() == 1) return std::move(tracks[0]);
        if (tracks.size() == 2) return MergeSequences(std::move(tracks[0]), std::move(tracks[1]));

        size_t totalSize = 0;
        for (const auto& track : tracks)
            totalSize += track.size();

        if (totalSize == 0) return {};

        std::vector<T> result;
        result.reserve(totalSize);

        struct TrackHead
        {
            T* ptr;
            T* end;
            size_t trackIdx;

            bool operator<(const TrackHead& other) const
            {
                if (ptr->tick != other.ptr->tick)
                    return ptr->tick > other.ptr->tick;
                return trackIdx > other.trackIdx;
            }
        };

        std::vector<TrackHead> heap;
        heap.reserve(tracks.size());

        for (size_t i = 0; i < tracks.size(); i++)
        {
            if (tracks[i].empty()) continue;
            heap.push_back({ tracks[i].data(), tracks[i].data() + tracks[i].size(), i });
        }

        std::make_heap(heap.begin(), heap.end());

        while (!heap.empty())
        {
            std::pop_heap(heap.begin(), heap.end());
            TrackHead& head = heap.back();

            result.emplace_back(std::move(*head.ptr));
            head.ptr++;

            if (head.ptr != head.end)
                std::push_heap(heap.begin(), heap.end());
            else
                heap.pop_back();
        }

        return result;
    }

    template <typename T>
    static void ApplyTempoEvents(uint16_t ppq, TempoMap* tempoMap, std::vector<T>& events)
    {
        static_assert(std::is_base_of_v<MIDIEvent, T>, "T must derive from MIDIEvent.");

        const double MAX_SAFE_SECS = 42949.0;
    
        for (auto& ev : events)
        {
            long tick = ev.tick;
            double secs = tempoMap->TicksToSecsFromMap(ppq, ev.tick);
            ev.tick = std::lround(std::min(secs, MAX_SAFE_SECS) * TIME_BASED_MULTIPLIER);
            // if constexpr (std::is_same_v<T, NoteEvent>)
            // {
            //     double endSecs = tempoMap->TicksToSecsFromMap(ppq, tick + ev.gate);
            //     double gate = endSecs - secs;
            //     ev.gate = std::lround(gate * TIME_BASED_MULTIPLIER);
            // }
        }
    }

    // static std::vector<std::vector<NoteEvent>> DistributeNotes(std::vector<NoteEvent>&& notes);
    static NoteSequence FlattenSequence(std::vector<NoteSequence>&& tracks);
    static std::vector<NoteSequence> DistributeNotes(NoteSequence&& notes);

    // Merge the tick-ordered tracks and split them per key in one pass. Doing the split during
    // the merge saves building and then re-reading a whole-file note sequence.
    static std::vector<NoteSequence> MergeAndDistribute(std::vector<NoteSequence>&& tracks);

    // Merge the tracks inside tick windows: each window is filled on its own thread into exact
    // per-key buffers, so no heap is shared between keys and nothing is reallocated.
    // `deduplicatedNotes`, when given, receives the number of identical notes collapsed while
    // the per-key lists are finalized. The collapse is part of the parallel sort so it costs no
    // extra serial pass over every note.
    // Reads the track store without consuming it, so a packed sequence can be converted to the
    // per-key layout while the packed renderer still draws from the same notes.
    static std::vector<NoteSequence> WindowedMergeAndDistribute(const std::vector<NoteSequence>& tracks, uint32_t maxTickHint = 0, size_t* deduplicatedNotes = nullptr);

    // Notes with the same tick, gate and key are the same rectangle: layered black MIDIs repeat
    // them by the million. Keeping one changes neither the sound nor the picture, and gives the
    // renderer far fewer quads to build.
    static size_t DeduplicateNotes(NoteSequence& notes)
    {
        const size_t count = notes.Size();
        if (count < 2) return 0;

        size_t kept = 0;
        size_t removed = 0;

        for (size_t i = 0; i < count; i++)
        {
            // packed carries the key, velocity, channel and track, so only notes that are
            // identical in every way are merged; a different instrument is left alone.
            if (kept > 0
                && notes.tick[kept - 1] == notes.tick[i]
                && notes.gate[kept - 1] == notes.gate[i]
                && notes.packed[kept - 1] == notes.packed[i])
            {
                removed++;
                continue;
            }

            if (kept != i)
            {
                notes.tick[kept] = notes.tick[i];
                notes.gate[kept] = notes.gate[i];
                notes.packed[kept] = notes.packed[i];
            }
            kept++;
        }

        if (removed > 0)
            notes.Resize(kept);

        return removed;
    }

    static size_t DeduplicateAll(std::vector<NoteSequence>& sequences)
    {
        size_t removed = 0;
        for (NoteSequence& notes : sequences)
            removed += DeduplicateNotes(notes);

        return removed;
    }

    static void ApplyTempoEvents(uint16_t ppq, TempoMap* tempoMap, NoteSequence& notes);
};