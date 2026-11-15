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

std::vector<NoteSequence> SequenceFuncs::WindowedMergeAndDistribute(std::vector<NoteSequence>&& tracks, uint32_t maxTickHint, size_t* deduplicatedNotes)
{
    std::vector<NoteSequence> result(MIDI_KEYS);
    if (deduplicatedNotes) *deduplicatedNotes = 0;
    if (tracks.empty()) return result;

    size_t totalNotes = 0;
    for (const NoteSequence& track : tracks)
        totalNotes += track.Size();

    if (totalNotes == 0) return result;

    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0) hardwareThreads = 8;
    const size_t workerCount = totalNotes < (1u << 20)
        ? 1
        : std::min<size_t>(MIDI_KEYS, hardwareThreads);

    // One track is already in tick order, so splitting it by key keeps that order. Going through
    // a merge here would allocate a full staging array for nothing; real black MIDIs are often a
    // single huge track, which made that copy the largest allocation in the load.
    if (tracks.size() == 1)
    {
        const NoteSequence& only = tracks[0];

        size_t counts[MIDI_KEYS]{};
        for (size_t i = 0; i < only.Size(); i++)
            counts[only.GetKey(i)]++;

        size_t cursor[MIDI_KEYS]{};
        for (size_t key = 0; key < MIDI_KEYS; key++)
            result[key].Resize(counts[key]);

        for (size_t i = 0; i < only.Size(); i++)
        {
            const uint8_t key = only.GetKey(i);
            const size_t pos = cursor[key]++;

            result[key].tick[pos] = only.tick[i];
            result[key].gate[pos] = only.gate[i];
            result[key].packed[pos] = only.packed[i];
        }

        std::atomic<size_t> removed{ 0 };
        auto dedupeOne = [&](size_t key)
            {
                removed.fetch_add(DeduplicateNotes(result[key]), std::memory_order_relaxed);
            };

        if (workerCount <= 1)
        {
            for (size_t key = 0; key < MIDI_KEYS; key++) dedupeOne(key);
        }
        else
        {
            std::atomic<size_t> nextKey{ 0 };
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (size_t i = 0; i < workerCount; i++)
            {
                workers.emplace_back([&]()
                    {
                        size_t key;
                        while ((key = nextKey.fetch_add(1, std::memory_order_relaxed)) < MIDI_KEYS)
                            dedupeOne(key);
                    });
            }
            for (auto& worker : workers)
                if (worker.joinable()) worker.join();
        }

        if (deduplicatedNotes) *deduplicatedNotes = removed.load(std::memory_order_relaxed);
        return result;
    }

    // The loader already knows the sequence length, which bounds every note tick, so it can skip
    // this whole pass; standalone callers pass 0 and get the scan.
    uint32_t maxTick = maxTickHint;
    if (maxTick == 0)
    {
        for (const NoteSequence& track : tracks)
        {
            for (size_t i = 0; i < track.Size(); i++)
            {
                if (track.tick[i] > maxTick) maxTick = track.tick[i];
            }
        }
    }

    // Split the timeline into windows. Notes from different tracks overlap across the whole file,
    // so a window is small enough to sort cheaply while still being an independent task.
    constexpr size_t MIN_WINDOWS = 64;
    const uint64_t tickSpan = (uint64_t)maxTick + 1;
    size_t windowCount = MIN_WINDOWS;
    while (windowCount < (1u << 14) && tickSpan / windowCount > (4u << 20))
        windowCount *= 2;

    const uint64_t windowTicks = std::max<uint64_t>(1,
        (tickSpan + windowCount - 1) / windowCount);
    const size_t bucketCount = (size_t)MIDI_KEYS * windowCount;

    // Count per worker first, then reduce. That removes the two serial passes over every note:
    // the workers also scatter into disjoint slices in track order, so the result is identical
    // to a single stable pass.
    const size_t scatterWorkers = std::min<size_t>(workerCount, tracks.size());
    std::vector<std::vector<uint32_t>> localCounts(
        scatterWorkers, std::vector<uint32_t>(bucketCount, 0));

    auto trackWindow = [&](uint32_t tick)
        {
            return (size_t)std::min<uint64_t>(windowCount - 1,
                (uint64_t)tick / windowTicks);
        };

    auto scatterWorker = [&](size_t worker)
        {
            const size_t begin = tracks.size() * worker / scatterWorkers;
            const size_t end = tracks.size() * (worker + 1) / scatterWorkers;
            auto& counts = localCounts[worker];

            for (size_t t = begin; t < end; t++)
            {
                const NoteSequence& track = tracks[t];
                for (size_t i = 0; i < track.Size(); i++)
                    counts[(size_t)track.GetKey(i) * windowCount + trackWindow(track.tick[i])]++;
            }
        };

    if (scatterWorkers <= 1)
    {
        for (size_t i = 0; i < scatterWorkers; i++) scatterWorker(i);
    }
    else
    {
        std::vector<std::thread> workers;
        workers.reserve(scatterWorkers);
        for (size_t i = 0; i < scatterWorkers; i++)
            workers.emplace_back(scatterWorker, i);
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    std::vector<size_t> offsets(bucketCount, 0);
    std::vector<size_t> bucketTotals(bucketCount, 0);
    for (size_t key = 0; key < MIDI_KEYS; key++)
    {
        size_t running = 0;
        for (size_t w = 0; w < windowCount; w++)
        {
            const size_t bucket = key * windowCount + w;
            offsets[bucket] = running;

            size_t count = 0;
            for (const auto& local : localCounts)
                count += local[bucket];
            bucketTotals[bucket] = count;
            running += count;
        }

        result[key].Resize(running);
    }

    // Each worker writes its own slice of every bucket, in track order.
    std::vector<std::vector<size_t>> cursors(scatterWorkers, std::vector<size_t>(bucketCount, 0));
    for (size_t bucket = 0; bucket < bucketCount; bucket++)
    {
        size_t at = offsets[bucket];
        for (size_t worker = 0; worker < scatterWorkers; worker++)
        {
            cursors[worker][bucket] = at;
            at += localCounts[worker][bucket];
        }
    }

    auto scatterNotes = [&](size_t worker)
        {
            const size_t begin = tracks.size() * worker / scatterWorkers;
            const size_t end = tracks.size() * (worker + 1) / scatterWorkers;
            auto& cursor = cursors[worker];

            for (size_t t = begin; t < end; t++)
            {
                const NoteSequence& track = tracks[t];
                for (size_t i = 0; i < track.Size(); i++)
                {
                    const size_t bucket =
                        (size_t)track.GetKey(i) * windowCount + trackWindow(track.tick[i]);
                    const size_t pos = cursor[bucket]++;
                    result[track.GetKey(i)].tick[pos] = track.tick[i];
                    result[track.GetKey(i)].gate[pos] = track.gate[i];
                    result[track.GetKey(i)].packed[pos] = track.packed[i];
                }
            }
        };

    if (scatterWorkers <= 1)
    {
        for (size_t i = 0; i < scatterWorkers; i++) scatterNotes(i);
    }
    else
    {
        std::vector<std::thread> workers;
        workers.reserve(scatterWorkers);
        for (size_t i = 0; i < scatterWorkers; i++)
            workers.emplace_back(scatterNotes, i);
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    // Sort each window by tick with a local histogram. The bins cover only this window's tick
    // range, so they stay cache-resident even though the whole file spans a million ticks.
    std::atomic<size_t> nextBucket{ 0 };
    auto sortWorker = [&]()
        {
            std::vector<uint32_t> histogram;
            std::vector<uint32_t> tmpTick;
            std::vector<uint32_t> tmpGate;
            std::vector<uint32_t> tmpPacked;

            size_t bucket;
            while ((bucket = nextBucket.fetch_add(1, std::memory_order_relaxed)) < bucketCount)
            {
                const size_t count = bucketTotals[bucket];
                if (count < 2) continue;

                const size_t key = bucket / windowCount;
                const size_t w = bucket % windowCount;
                const size_t start = offsets[bucket];
                const uint64_t base = windowTicks * w;
                const size_t bins = (size_t)std::min<uint64_t>(windowTicks, tickSpan - base);
                if (bins == 0) continue;

                NoteSequence& notes = result[key];
                histogram.assign(bins, 0);

                for (size_t i = 0; i < count; i++)
                {
                    const size_t bin = (size_t)((uint64_t)notes.tick[start + i] - base);
                    histogram[bin < bins ? bin : bins - 1]++;
                }

                uint32_t running = 0;
                for (uint32_t& n : histogram)
                {
                    const uint32_t c = n;
                    n = running;
                    running += c;
                }

                tmpTick.resize(count);
                tmpGate.resize(count);
                tmpPacked.resize(count);

                for (size_t i = 0; i < count; i++)
                {
                    const uint32_t tick = notes.tick[start + i];
                    const size_t bin = (size_t)((uint64_t)tick - base);
                    const size_t pos = histogram[bin < bins ? bin : bins - 1]++;
                    tmpTick[pos] = tick;
                    tmpGate[pos] = notes.gate[start + i];
                    tmpPacked[pos] = notes.packed[start + i];
                }

                std::copy(tmpTick.begin(), tmpTick.end(), notes.tick.begin() + start);
                std::copy(tmpGate.begin(), tmpGate.end(), notes.gate.begin() + start);
                std::copy(tmpPacked.begin(), tmpPacked.end(), notes.packed.begin() + start);
            }
        };

    if (workerCount <= 1)
    {
        sortWorker();
    }
    else
    {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; i++)
            workers.emplace_back(sortWorker);
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    // The windows are contiguous per key, so one final pass per key removes identical rectangles.
    std::atomic<size_t> removed{ 0 };
    auto dedupeKey = [&](size_t key)
        {
            removed.fetch_add(DeduplicateNotes(result[key]), std::memory_order_relaxed);
        };

    if (workerCount <= 1)
    {
        for (size_t key = 0; key < MIDI_KEYS; key++) dedupeKey(key);
    }
    else
    {
        std::atomic<size_t> nextKey{ 0 };
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; i++)
        {
            workers.emplace_back([&]()
                {
                    size_t key;
                    while ((key = nextKey.fetch_add(1, std::memory_order_relaxed)) < MIDI_KEYS)
                        dedupeKey(key);
                });
        }
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    if (deduplicatedNotes) *deduplicatedNotes = removed.load(std::memory_order_relaxed);
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