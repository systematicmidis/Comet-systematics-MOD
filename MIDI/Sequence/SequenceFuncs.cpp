#include "SequenceFuncs.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <array>
#include <vector>
#include <numeric>

NoteSequence SequenceFuncs::FlattenSequence(std::vector<NoteSequence>&& tracks)
{
    if (tracks.empty()) return {};
    if (tracks.size() == 1) return std::move(tracks[0]);

    size_t totalSize = 0;
    for (const auto& track : tracks)
        totalSize += track.Size();

    NoteSequence result;
    if (totalSize == 0) return result;
    result.Reserve(totalSize);

    struct TrackHead
    {
        size_t elementIdx;
        uint32_t trackIdx;
        uint32_t tick;

        bool operator<(const TrackHead& other) const
        {
            if (tick != other.tick)
                return tick > other.tick;
            return trackIdx > other.trackIdx;
        }
    };

    std::vector<TrackHead> heap;
    heap.reserve(tracks.size());

    for (size_t i = 0; i < tracks.size(); i++)
    {
        if (!tracks[i].Empty())
        {
            heap.push_back({ 0, static_cast<uint32_t>(i), tracks[i].tick[0] });
        }
    }

    std::make_heap(heap.begin(), heap.end());

    while (!heap.empty())
    {
        std::pop_heap(heap.begin(), heap.end());
        TrackHead& head = heap.back();

        auto& src = tracks[head.trackIdx];
        size_t idx = head.elementIdx;

        result.Emplace(
            src.GetTrack(idx),
            src.GetChannel(idx),
            src.tick[idx],
            src.GetKey(idx),
            src.gate[idx],
            src.GetVelocity(idx)
        );

        head.elementIdx++;
        if (head.elementIdx < src.Size())
        {
            head.tick = src.tick[head.elementIdx];
            std::push_heap(heap.begin(), heap.end());
        }
        else
        {
            heap.pop_back();
        }
    }

    return result;
}

std::vector<NoteSequence> SequenceFuncs::WindowedMergeAndDistribute(std::vector<NoteSequence>&& tracks)
{
    std::vector<NoteSequence> result(MIDI_KEYS);
    if (tracks.empty()) return result;

    size_t totalNotes = 0;
    for (const NoteSequence& track : tracks)
        totalNotes += track.Size();

    if (totalNotes == 0) return result;

    // One track is already in tick order, so splitting it by key keeps that order. Going through
    // the windowed merge here would allocate a full ref array for nothing; real black MIDIs are
    // often a single huge track, which made that copy the largest allocation in the load.
    if (tracks.size() == 1)
    {
        const NoteSequence& only = tracks[0];

        size_t counts[MIDI_KEYS]{};
        for (size_t i = 0; i < only.Size(); i++)
            counts[only.GetKey(i)]++;

        size_t cursor[MIDI_KEYS]{};
        for (size_t key = 0; key < MIDI_KEYS; key++)
        {
            result[key].Resize(counts[key]);
            cursor[key] = 0;
        }

        for (size_t i = 0; i < only.Size(); i++)
        {
            const uint8_t key = only.GetKey(i);
            const size_t pos = cursor[key]++;

            result[key].tick[pos] = only.tick[i];
            result[key].gate[pos] = only.gate[i];
            result[key].packed[pos] = only.packed[i];
        }

        return result;
    }

    uint32_t maxTick = 0;
    for (const NoteSequence& track : tracks)
    {
        for (size_t i = 0; i < track.Size(); i++)
        {
            if (track.tick[i] > maxTick) maxTick = track.tick[i];
        }
    }

    constexpr size_t WINDOWS = 64;
    const uint32_t windowTicks =
        std::max<uint32_t>(1, (maxTick + (uint32_t)WINDOWS - 1) / (uint32_t)WINDOWS);

    // Exact size per (key, window), then one running offset per key: the output buffers never
    // grow, and notes can be written straight into their window while they are being scattered.
    std::vector<size_t> counts((size_t)MIDI_KEYS * WINDOWS, 0);
    for (const NoteSequence& track : tracks)
    {
        for (size_t i = 0; i < track.Size(); i++)
        {
            const size_t w = std::min<size_t>(WINDOWS - 1, track.tick[i] / windowTicks);
            counts[(size_t)track.GetKey(i) * WINDOWS + w]++;
        }
    }

    std::vector<size_t> offsets((size_t)MIDI_KEYS * WINDOWS, 0);
    for (size_t key = 0; key < MIDI_KEYS; key++)
    {
        size_t running = 0;
        for (size_t w = 0; w < WINDOWS; w++)
        {
            offsets[key * WINDOWS + w] = running;
            running += counts[key * WINDOWS + w];
        }

        result[key].Resize(running);
    }

    // Scatter each note into its final slot. Notes enter a bucket in (track, index) order, which
    // is exactly the heap merge's tie-break, so a stable sort on tick alone reproduces its order.
    std::vector<size_t> cursor(offsets);
    for (uint32_t t = 0; t < (uint32_t)tracks.size(); t++)
    {
        const NoteSequence& track = tracks[t];
        for (size_t i = 0; i < track.Size(); i++)
        {
            const size_t w = std::min<size_t>(WINDOWS - 1, track.tick[i] / windowTicks);
            const uint8_t key = track.GetKey(i);
            const size_t pos = cursor[(size_t)key * WINDOWS + w]++;

            result[key].tick[pos] = track.tick[i];
            result[key].gate[pos] = track.gate[i];
            result[key].packed[pos] = track.packed[i];
        }
    }

    // The previous version laid out a 12-byte ref per note before it could sort. Sorting 4-byte
    // indices over the already-scattered ticks uses a third of that, and the scratch buffers are
    // reused, so peak memory is one full note array instead of two.
    std::vector<uint32_t> order;
    std::vector<uint32_t> tmpTick;
    std::vector<uint32_t> tmpGate;
    std::vector<uint32_t> tmpPacked;

    for (size_t key = 0; key < MIDI_KEYS; key++)
    {
        NoteSequence& out = result[key];

        for (size_t w = 0; w < WINDOWS; w++)
        {
            const size_t start = offsets[key * WINDOWS + w];
            const size_t n = counts[key * WINDOWS + w];
            if (n < 2) continue;

            order.resize(n);
            for (size_t i = 0; i < n; i++)
                order[i] = (uint32_t)i;

            const uint32_t* ticks = out.tick.data() + start;
            std::stable_sort(order.begin(), order.end(),
                [ticks](uint32_t a, uint32_t b) { return ticks[a] < ticks[b]; });

            bool sorted = true;
            for (size_t i = 0; i < n; i++)
            {
                if (order[i] != i)
                {
                    sorted = false;
                    break;
                }
            }

            if (sorted) continue;

            tmpTick.resize(n);
            tmpGate.resize(n);
            tmpPacked.resize(n);

            for (size_t i = 0; i < n; i++)
            {
                const size_t src = start + order[i];
                tmpTick[i] = out.tick[src];
                tmpGate[i] = out.gate[src];
                tmpPacked[i] = out.packed[src];
            }

            std::copy(tmpTick.begin(), tmpTick.end(), out.tick.begin() + start);
            std::copy(tmpGate.begin(), tmpGate.end(), out.gate.begin() + start);
            std::copy(tmpPacked.begin(), tmpPacked.end(), out.packed.begin() + start);
        }
    }

    return result;
}

std::vector<NoteSequence> SequenceFuncs::MergeAndDistribute(std::vector<NoteSequence>&& tracks)
{
    std::vector<NoteSequence> result(MIDI_KEYS);
    if (tracks.empty()) return result;

    // Count per key first: reserving exact sizes means no reallocation while notes are copied.
    size_t counts[MIDI_KEYS]{};
    for (const NoteSequence& track : tracks)
    {
        for (size_t i = 0; i < track.Size(); i++)
            counts[track.GetKey(i)]++;
    }

    for (size_t key = 0; key < MIDI_KEYS; key++)
        result[key].Reserve(counts[key]);

    auto emit = [&result](const NoteSequence& src, size_t idx)
    {
        const uint8_t key = src.GetKey(idx);
        result[key].Emplace(src.GetTrack(idx), src.GetChannel(idx), src.tick[idx], key,
            src.gate[idx], src.GetVelocity(idx));
    };

    if (tracks.size() == 1)
    {
        // A single track is already in tick order, so it only has to be split.
        const NoteSequence& only = tracks[0];
        for (size_t i = 0; i < only.Size(); i++)
            emit(only, i);

        return result;
    }

    struct TrackHead
    {
        size_t elementIdx;
        uint32_t trackIdx;
        uint32_t tick;

        bool operator<(const TrackHead& other) const
        {
            if (tick != other.tick)
                return tick > other.tick;
            return trackIdx > other.trackIdx;
        }
    };

    std::vector<TrackHead> heap;
    heap.reserve(tracks.size());

    for (size_t i = 0; i < tracks.size(); i++)
    {
        if (!tracks[i].Empty())
            heap.push_back({ 0, static_cast<uint32_t>(i), tracks[i].tick[0] });
    }

    std::make_heap(heap.begin(), heap.end());

    while (!heap.empty())
    {
        std::pop_heap(heap.begin(), heap.end());
        TrackHead& head = heap.back();

        const NoteSequence& src = tracks[head.trackIdx];
        emit(src, head.elementIdx);

        head.elementIdx++;
        if (head.elementIdx < src.Size())
        {
            head.tick = src.tick[head.elementIdx];
            std::push_heap(heap.begin(), heap.end());
        }
        else
        {
            heap.pop_back();
        }
    }

    return result;
}

std::vector<NoteSequence> SequenceFuncs::DistributeNotes(NoteSequence&& notes)
{
    std::vector<NoteSequence> result(MIDI_KEYS);
    const size_t numNotes = notes.Size();

    size_t counts[MIDI_KEYS]{};
    for (size_t n = 0; n < numNotes; n++)
        counts[notes.GetKey(n)]++;

    for (size_t n = 0; n < MIDI_KEYS; n++)
        result[n].Reserve(counts[n]);

    for (size_t i = 0; i < numNotes; ++i)
    {
        uint8_t noteVal = notes.GetKey(i);

        result[noteVal].Emplace(
            notes.GetTrack(i),
            notes.GetChannel(i),
            notes.tick[i],
            noteVal,
            notes.gate[i],
            notes.GetVelocity(i)
        );
    }

    return result;
}

void SequenceFuncs::ApplyTempoEvents(uint16_t ppq, TempoMap* tempoMap, NoteSequence& notes)
{
    const double MAX_SAFE_SECS = 42949.0;
    const size_t size = notes.Size();

    for (size_t i = 0; i < size; ++i)
    {
        long tick = notes.tick[i];

        double secs = tempoMap->TicksToSecsFromMap(ppq, tick);
        notes.tick[i] = std::lround(std::min(secs, MAX_SAFE_SECS) * TIME_BASED_MULTIPLIER);

        double endSecs = tempoMap->TicksToSecsFromMap(ppq, tick + notes.gate[i]);
        double gateSecs = endSecs - secs;
        notes.gate[i] = std::lround(gateSecs * TIME_BASED_MULTIPLIER);
    }
}