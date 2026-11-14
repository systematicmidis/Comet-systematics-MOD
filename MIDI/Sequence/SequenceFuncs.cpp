#include "SequenceFuncs.h"
#include <array>
#include <vector>

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

    // Exact size per (key, window), then one running offset per key: the buffers never grow.
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

    auto fillWindow = [&tracks, &result, &offsets, windowTicks](size_t w)
    {
        struct Ref
        {
            uint32_t tick;
            uint32_t track;
            uint32_t index;
        };

        const uint32_t begin = (uint32_t)(w * windowTicks);
        const uint32_t end = (w + 1 == WINDOWS) ? UINT32_MAX : (uint32_t)((w + 1) * windowTicks);

        std::vector<Ref> refs;
        refs.reserve(4096);

        for (uint32_t t = 0; t < (uint32_t)tracks.size(); t++)
        {
            const NoteSequence& track = tracks[t];

            size_t lo = 0;
            size_t hi = track.Size();
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (track.tick[mid] < begin) lo = mid + 1;
                else hi = mid;
            }

            for (size_t i = lo; i < track.Size() && track.tick[i] < end; i++)
                refs.push_back({ track.tick[i], t, (uint32_t)i });
        }

        std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b)
            {
                if (a.tick != b.tick) return a.tick < b.tick;
                if (a.track != b.track) return a.track < b.track;
                return a.index < b.index;
            });

        size_t cursor[MIDI_KEYS]{};
        for (size_t key = 0; key < MIDI_KEYS; key++)
            cursor[key] = offsets[key * WINDOWS + w];

        for (const Ref& ref : refs)
        {
            const NoteSequence& src = tracks[ref.track];
            const uint8_t key = src.GetKey(ref.index);
            const size_t out = cursor[key]++;

            result[key].tick[out] = src.tick[ref.index];
            result[key].gate[out] = src.gate[ref.index];
            result[key].packed[out] = src.packed[ref.index];
        }
    };

    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned>(threads, (unsigned)WINDOWS);

    std::atomic<size_t> nextWindow{ 0 };
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (unsigned w = 0; w < threads; w++)
    {
        workers.emplace_back([&fillWindow, &nextWindow]()
            {
                size_t window;
                while ((window = nextWindow.fetch_add(1, std::memory_order_relaxed)) < WINDOWS)
                    fillWindow(window);
            });
    }

    for (std::thread& worker : workers)
    {
        if (worker.joinable()) worker.join();
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