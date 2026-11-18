// idk how but this is blazingly fast

#include "AudioThread.h"
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <queue>
#include <functional>
#include "MIDI/TempoMap.h"
#include "MIDI/MIDIDefs.h"

struct PrecalculatedEvent
{
    long tick;
    uint32_t message;
};

void AudioThread::Start(std::shared_ptr<MIDISequence> seq, std::shared_ptr<MIDITimer> timer)
{
    if (!seq || !timer) return;

    Stop();
    midiOut->Initialize();
    stopFlag = false;

    std::thread audioThread([this, seq, timer]() {
        auto* tempoMap = seq->GetTempoMap();
        		const auto resolution = seq->resolution;

		// Packed giant files cannot be flattened into one event vector: 100M notes would need a
		// multi-gigabyte allocation and a full stable sort. Stream note-ons from the already
		// tick-sorted per-track cursors and keep only the currently sounding note-offs in a heap.
		if (seq->packed)
		{
			struct NoteOff
			{
				long tick;
				uint32_t message;
				bool operator>(const NoteOff& other) const { return tick > other.tick; }
			};

			const auto& tracks = seq->trackNotes;
			const SparseSeekIndex& index = seq->GetSparseSeekIndex();
			std::vector<size_t> cursors(tracks.size(), 0);
			std::priority_queue<NoteOff, std::vector<NoteOff>, std::greater<NoteOff>> noteOffs;
			size_t eventCursor = 0;

			auto sendControlEventsUpTo = [&](long tick)
			{
				while (eventCursor < seq->mergedEvents.size() && seq->mergedEvents[eventCursor].tick <= tick)
				{
					const uint32_t message = seq->mergedEvents[eventCursor++].message;
					if ((message & 0xF0) != 0x80 && (message & 0xF0) != 0x90)
						midiOut->SendEvent(message);
				}
			};

			auto seekCursors = [&](long tick)
			{
				for (size_t track = 0; track < tracks.size(); ++track)
				{
					if (index.TrackCount() == tracks.size())
						cursors[track] = index.LowerBound(track, (uint32_t)std::max(0L, tick), tracks);
					else
						cursors[track] = (size_t)(std::lower_bound(tracks[track].tick.begin(),
							tracks[track].tick.end(), (uint32_t)std::max(0L, tick)) - tracks[track].tick.begin());
				}

				while (!noteOffs.empty()) noteOffs.pop();
				eventCursor = 0;
				sendControlEventsUpTo(tick);
			};

			auto playUpTo = [&](long tick)
			{
				while (!noteOffs.empty() && noteOffs.top().tick <= tick)
				{
					midiOut->SendEvent(noteOffs.top().message);
					noteOffs.pop();
				}

				sendControlEventsUpTo(tick);

				for (size_t track = 0; track < tracks.size(); ++track)
				{
					const NoteSequence& notes = tracks[track];
					size_t& cursor = cursors[track];
					while (cursor < notes.Size() && (long)notes.tick[cursor] <= tick)
					{
						const uint32_t noteTick = notes.tick[cursor];
						const uint8_t channel = notes.GetChannel(cursor);
						const uint8_t key = notes.GetKey(cursor);
						const uint8_t velocity = notes.GetVelocity(cursor);

						midiOut->SendEvent(0x90u | channel | ((uint32_t)key << 8) | ((uint32_t)velocity << 16));
						noteOffs.push({ (long)noteTick + (long)notes.gate[cursor],
							0x80u | channel | ((uint32_t)key << 8) });
						++cursor;
					}
				}
			};

			{
				double currentTime = timer->IsPaused() ? timer->GetPauseTime() : timer->Elapsed();
				const long startTick = seq->timeBased
					? (long)(currentTime * TIME_BASED_MULTIPLIER)
					: (long)tempoMap->SecsToTicksFromMap(resolution, currentTime);
				seekCursors(startTick);
			}

			while (!stopFlag)
			{
				if (audioMuted)
				{
					std::this_thread::sleep_for(std::chrono::microseconds(500));
					continue;
				}

				const bool navigated = timer->HasNavigatedRecently();
				const long currentTick = (long)tempoMap->SecsToTicksFromMap(resolution, timer->Elapsed());
				const long currPos = seq->timeBased
					? (long)(timer->Elapsed() * TIME_BASED_MULTIPLIER) : currentTick;

				if (navigated)
				{
					ResetEvents();
					const long targetTick = seq->timeBased
						? (long)(timer->GetPauseTime() * TIME_BASED_MULTIPLIER)
						: (long)tempoMap->SecsToTicksFromMap(resolution, timer->GetPauseTime());
					seekCursors(targetTick);
				}

				if (navigated || timer->PauseRequestedRecently())
				{
					for (int ch = 0; ch < 16; ch++)
						midiOut->SendEvent(0xB0u | ch | (123u << 8));
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				if (!timer->IsPaused())
					playUpTo(currPos);

				std::this_thread::sleep_for(std::chrono::microseconds(500));
			}

			ResetEvents();
			threadWorking = false;
			return;
		}

		// `seq->notes` counts every repeated copy for display; the events we build are only the
        // unique merged notes. Reserving from the display total would try to allocate terabytes.
        const auto& noteLists = seq->mergedNotes.empty() ? seq->trackNotes : seq->mergedNotes;
        size_t eventCount = seq->mergedEvents.size();
        for (const auto& notes : noteLists)
            eventCount += notes.Size();

        std::vector<PrecalculatedEvent> events;
        events.reserve(eventCount * 2);

        for (const auto& ev : seq->mergedEvents)
        {
            events.push_back({ ev.tick, ev.message });
        }

        for (const NoteSequence& notes : noteLists)
        {
            for (size_t j = 0; j < notes.Size(); j++)
            {
                uint32_t nTick = notes.tick[j];
                uint32_t nGate = notes.gate[j];
                uint8_t nNote = notes.GetKey(j);
                uint8_t nChannel = notes.GetChannel(j);
                uint8_t nVel = notes.GetVelocity(j);

                events.push_back({
                    (long)nTick,
                    0x90u | nChannel | ((uint32_t)nNote << 8) | ((uint32_t)nVel << 16)
                    });

                events.push_back({
                    (long)nTick + (long)nGate,
                    0x80u | nChannel | ((uint32_t)nNote << 8)
                    });
            }
        }

        std::stable_sort(events.begin(), events.end(), [](const PrecalculatedEvent& a, const PrecalculatedEvent& b) { return a.tick < b.tick; });

        ResetEvents();

        size_t cursor = 0;
        uint32_t msg = 0;

        auto syncToTick = [&](long tick)
            {
                cursor = (size_t)(std::lower_bound(
                    events.begin(),
                    events.end(),
                    tick,
                    [](const PrecalculatedEvent& ev, long pos)
                    {
                        return ev.tick < pos;
                    }) - events.begin());

                for (const auto& ev : events)
                {
                    if (ev.tick > tick) break;
                    if ((ev.message & 0xF0) == 0x80 || (ev.message & 0xF0) == 0x90) continue;
                    midiOut->SendEvent(ev.message);
                }
            };

        {
            double currentTime = timer->IsPaused() ? timer->GetPauseTime() : timer->Elapsed();
            const long startTick = seq->timeBased
                ? (long)(currentTime * TIME_BASED_MULTIPLIER)
                : (long)tempoMap->SecsToTicksFromMap(resolution, currentTime);

            syncToTick(startTick);
        }

        while (!stopFlag)
        {
            if (audioMuted)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }

            const bool navigated = timer->HasNavigatedRecently();
            const long currentTick = (long)tempoMap->SecsToTicksFromMap(resolution, timer->Elapsed());
            long currPos = seq->timeBased ? (long)(timer->Elapsed() * TIME_BASED_MULTIPLIER) : (double)currentTick;

            if (navigated)
            {
                ResetEvents();

                long targetTick = seq->timeBased
                    ? (long)(timer->GetPauseTime() * TIME_BASED_MULTIPLIER)
                    : (long)tempoMap->SecsToTicksFromMap(resolution, timer->GetPauseTime());

                syncToTick(targetTick);
            }

            if (navigated || timer->PauseRequestedRecently())
            {
                for (int ch = 0; ch < 16; ch++)
                {
                    midiOut->SendEvent(0xB0 | ch | (123 << 8));
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (!timer->IsPaused())
            {
                while (cursor < events.size() && events[cursor].tick <= currPos)
                {
                    if (stopFlag || audioMuted) break;

                    const auto& ev = events[cursor++];
                    midiOut->SendEvent(ev.message);
                }
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        ResetEvents();

        threadWorking = false;
    });

    this->audioThread = std::move(audioThread);
    threadWorking = true;
}

void AudioThread::ResetEvents()
{
    // hardcoded way to reset CC
    for (int ch = 0; ch < 16; ch++)
    {
        midiOut->SendEvent(0xB0u | ch);
        midiOut->SendEvent(0xB0u | ch | (1u << 8u));
        midiOut->SendEvent(0xB0u | ch | (2u << 8u));
        midiOut->SendEvent(0xB0u | ch | (4u << 8u));
        midiOut->SendEvent(0xB0u | ch | (5u << 8u));
        midiOut->SendEvent(0xB0u | ch | (7u << 8u) | (100u << 16u)); // volume
        midiOut->SendEvent(0xB0u | ch | (8u << 8u) | (64u << 16u));
        midiOut->SendEvent(0xB0u | ch | (10u << 8u) | (64u << 16u)); // pan
        midiOut->SendEvent(0xB0u | ch | (11u << 8u) | (127u << 16u)); // expression

        // sustain, portamento, sostenuto, soft pedal, etc..
        for (unsigned int cc = 64; cc <= 69; cc++)
        {
            midiOut->SendEvent(0xB0u | ch | (cc << 8u));
        }

        for (unsigned int cc = 70; cc <= 79; cc++)
        {
            midiOut->SendEvent(0xB0u | ch | (cc << 8u) | (64u << 16u)); // sound controllers
        }

        midiOut->SendEvent(0xB0u | ch | (91u << 8u) | (40u << 16u)); // reverb

        for (unsigned int cc = 92; cc <= 95; cc++)
        {
            midiOut->SendEvent(0xB0u | ch | (cc << 8u));
        }

        midiOut->SendEvent(0xB0u | ch | (101u << 8u) | (127u << 16u));
        midiOut->SendEvent(0xB0u | ch | (100u << 8u) | (127u << 16u));
        midiOut->SendEvent(0xB0u | ch | (99u << 8u) | (127u << 16u));
        midiOut->SendEvent(0xB0u | ch | (98u << 8u) | (127u << 16u));

        // just in case, send the "reset all controllers" event too
        midiOut->SendEvent(0xB0 | ch | (121 << 8));
        midiOut->SendEvent(0xB0 | ch | (123 << 8));
    }
}