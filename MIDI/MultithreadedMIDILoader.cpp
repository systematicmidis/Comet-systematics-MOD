#include "MultithreadedMIDILoader.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <thread>
#include "Sequence/SequenceFuncs.h"
#include "TempoMap.h"
#include "IO/ArchiveLoader.h"

namespace
{
	// A bounds-checked cursor over one track's bytes. Reads past the end are clamped, so a damaged
	// chunk can never run off into the next track's data.
	struct ByteCursor
	{
		const uint8_t* p = nullptr;
		const uint8_t* end = nullptr;

		bool AtEnd() const { return p >= end; }
		size_t Remaining() const { return static_cast<size_t>(end - p); }

		uint8_t Byte()
		{
			return p < end ? *p++ : 0;
		}

		void Skip(size_t count)
		{
			p = count >= Remaining() ? end : p + count;
		}

		uint32_t VLQ()
		{
			uint32_t value = 0;
			for (int i = 0; i < 4 && p < end; i++)
			{
				const uint8_t b = *p++;
				value = (value << 7) | (b & 0x7F);
				if ((b & 0x80) == 0) break;
			}
			return value;
		}
	};

	std::filesystem::path Utf8Path(const std::string& utf8)
	{
		return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
	}
}

MultithreadedMIDILoader::MultithreadedMIDILoader(const char* file) : AbstractMIDILoader(file)
{
	this->file = std::string(file);
	is = nullptr;
}

MultithreadedMIDILoader::MultithreadedMIDILoader(std::shared_ptr<InputStream> is)
	: MultithreadedMIDILoader("")
{
	this->is = is;
}

std::shared_ptr<MIDISequence> MultithreadedMIDILoader::Load(bool timeBasedLoading)
{
	running = true;
	tracksProcessed = 0;
	archiveDone = 0;
	archiveTotal = 0;

	const uint8_t* data = nullptr;
	size_t dataSize = 0;

	if (!file.empty())
	{
		const std::filesystem::path path = Utf8Path(file);

		if (ArchiveLoader::IsArchive(path))
		{
			SetName("Decompressing archive...");
			AddBar([this]()
				{
					const uint64_t total = archiveTotal.load(std::memory_order_relaxed);
					return total > 0 ? (double)archiveDone.load(std::memory_order_relaxed) / (double)total : 0.0;
				});

			ArchiveLoader::Result extracted = ArchiveLoader::Extract(path, &hasStopped, &archiveDone, &archiveTotal);
			ClearBars();

			if (!running) return seq = std::make_shared<MIDISequence>();
			if (!extracted.ok) throw std::runtime_error(extracted.error);

			std::cout << "Extracted \"" << extracted.entryName << "\" (" << extracted.data.size() << " bytes)" << std::endl;
			ownedBytes = std::move(extracted.data);
			data = ownedBytes.data();
			dataSize = ownedBytes.size();
			seq = std::make_shared<MIDISequence>("TODO");
		}
		else
		{
			std::string mapError;
			mappedFile = std::make_unique<MappedFile>();
			if (mappedFile->Open(path, mapError))
			{
				mappedFile->Prefetch();
				data = mappedFile->Data();
				dataSize = mappedFile->Size();
			}
			else
			{
				// Mapping can fail for unusual filesystems; read it once instead of giving up.
				std::cout << "Memory mapping failed (" << mapError << "), reading the file instead." << std::endl;
				mappedFile.reset();

				std::ifstream stream(path, std::ios::binary);
				if (!stream.is_open()) throw std::runtime_error("Failed to open file");

				stream.seekg(0, std::ios::end);
				const std::streamoff fileSize = stream.tellg();
				stream.seekg(0, std::ios::beg);
				if (fileSize <= 0) throw std::runtime_error("The file is empty.");

				ownedBytes.resize((size_t)fileSize);
				stream.read(reinterpret_cast<char*>(ownedBytes.data()), fileSize);
				if (!stream) throw std::runtime_error("Failed to read file");

				data = ownedBytes.data();
				dataSize = ownedBytes.size();
			}

			seq = std::make_shared<MIDISequence>("TODO");
		}
	}
	else
	{
		std::cout << "Loading stream" << std::endl;
		seq = std::make_shared<MIDISequence>();

		// A stream has no mapping to parse in place, so take its bytes once.
		std::shared_ptr<std::ifstream> stream = is->GetStream();
		stream->clear();
		stream->seekg(0, std::ios::beg);
		ownedBytes.assign(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>());
		if (ownedBytes.empty()) throw std::runtime_error("The stream is empty.");

		data = ownedBytes.data();
		dataSize = ownedBytes.size();
	}

	SetName("Loading MIDI File...");

#pragma region Header Parse

	if (dataSize < 14 || ToInt(const_cast<uint8_t*>(data)) != 0x4d546864)
		throw std::runtime_error("Invalid header");
	if (ToInt(const_cast<uint8_t*>(data + 4)) != 6)
		throw std::runtime_error("Invalid header length");

	uint8_t* hdrP = const_cast<uint8_t*>(data + 8);
	const uint16_t format = ToShort(hdrP); hdrP += 2;
	const uint16_t tracks = ToShort(hdrP); hdrP += 2;
	seq->trackCount = tracks;
	seq->resolution = ToShort(hdrP);
	(void)format;

#pragma endregion

#pragma region Track Indexing

	std::vector<RawTrackChunk> rawChunks;
	rawChunks.reserve(tracks);

	{
		uint16_t i = 0;
		const auto& progress = [this, &i, tracks]() { return tracks == 0 ? 1.0 : (double)i / (double)tracks; };
		AddBar(progress);

		size_t offset = 14;
		for (; i < tracks; ++i)
		{
			SetName((std::string("Indexing track ") + std::to_string(i + 1) + "/" + std::to_string(tracks)).c_str());

			if (!running)
			{
				ClearBars();
				return seq;
			}

			// Some writers pad between chunks; find the next MTrk instead of assuming it is here.
			while (offset + 8 <= dataSize && std::memcmp(data + offset, "MTrk", 4) != 0) offset++;
			if (offset + 8 > dataSize) break;

			size_t trackLength = (size_t)ToInt(const_cast<uint8_t*>(data + offset + 4));
			const size_t start = offset + 8;
			if (start + trackLength > dataSize)
			{
				std::cout << "  Track " << (i + 1) << " is shorter than its header says; reading to the end." << std::endl;
				trackLength = dataSize - start;
			}

			RawTrackChunk chunk;
			chunk.data = data + start;
			chunk.length = trackLength;
			chunk.index = i;
			rawChunks.push_back(chunk);

			offset = start + trackLength;
		}

		ClearBars();
	}

#pragma endregion

#pragma region Parallel Track Parsing

	const size_t trackCount = rawChunks.size();
	std::vector<ParsedTrack> parsedTrackResults(trackCount);

	if (trackCount > 0)
	{
		unsigned int hardwareThreads = std::thread::hardware_concurrency();
		if (hardwareThreads == 0) hardwareThreads = 16;
		const size_t numThreads = std::min<size_t>(trackCount, hardwareThreads);

		AddBar([this, trackCount]()
			{
				return trackCount == 0 ? 1.0
					: (double)tracksProcessed.load(std::memory_order_relaxed) / (double)trackCount;
			});

		std::atomic<size_t> nextTrack{ 0 };
		std::vector<std::thread> workers;
		workers.reserve(numThreads);

		for (size_t workerIdx = 0; workerIdx < numThreads; workerIdx++)
		{
			workers.emplace_back([this, &rawChunks, &parsedTrackResults, &nextTrack, trackCount]()
				{
					size_t idx;
					while ((idx = nextTrack.fetch_add(1, std::memory_order_relaxed)) < trackCount)
					{
						if (!running) break;
						parsedTrackResults[idx] = ParseTrackData(rawChunks[idx]);
						tracksProcessed.fetch_add(1, std::memory_order_relaxed);
					}
				});
		}

		for (auto& worker : workers)
		{
			if (worker.joinable()) worker.join();
		}

		ClearBars();
	}

#pragma endregion

#pragma region Sequential Assembly

	std::vector<int> illegalTracks{};

	int noteTrackIdx = 0;
	int trackIdx = 0;

	std::vector<NoteSequence> notesToMerge;
	notesToMerge.reserve(trackCount);

	std::vector<std::vector<MIDIMessageEvent>> eventsToMerge;
	eventsToMerge.reserve(trackCount);

	for (auto& trackRes : parsedTrackResults)
	{
		if (!running) return seq;
		seq->length = std::max(seq->length, trackRes.length);

		if (trackRes.multiChannel)
			illegalTracks.push_back(trackIdx);

		if (!trackRes.notes.Empty())
		{
			seq->notes += trackRes.notes.Size();

			const uint32_t trackMask = NOTE_TRACK_MASK << NOTE_TRACK_BIT_IDX;
			for (uint32_t& packed : trackRes.notes.packed)
				packed = (packed & ~trackMask)
				| ((noteTrackIdx & NOTE_TRACK_MASK) << NOTE_TRACK_BIT_IDX);

			// Names are pushed in this same order, so they line up with the track indices that get
			// baked into the note data here.
			seq->noteTrackNames.push_back(trackRes.name);
			noteTrackIdx++;
			notesToMerge.push_back(std::move(trackRes.notes));
		}

		if (!trackRes.messages.empty())
		{
			eventsToMerge.push_back(std::move(trackRes.messages));
		}

		if (!trackRes.tempos.empty())
		{
			seq->tempos.insert(seq->tempos.end(), trackRes.tempos.begin(), trackRes.tempos.end());
		}

		if (!trackRes.timeSignatures.empty())
		{
			seq->timeSignatures.insert(seq->timeSignatures.end(), trackRes.timeSignatures.begin(), trackRes.timeSignatures.end());
		}
		else
		{
			seq->timeSignatures.emplace_back(0, 4, 4);
		}

		trackIdx++;
	}
#pragma endregion

#pragma region Post-processing & Merging
	seq->noteTrackCount = noteTrackIdx;
	std::cout << "\nLoaded all tracks, sorting tempo events." << std::endl;
	SetName("Starting event preprocessing");
	auto& tempos = seq->tempos;
	auto& timeSignatures = seq->timeSignatures;
	// These hold one entry per tempo/time-signature change, so they are tiny. Sorting them in
	// parallel costs more in thread-pool setup than the sort itself on every single load.
	std::sort(tempos.begin(), tempos.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
	std::sort(timeSignatures.begin(), timeSignatures.end(), [](auto& a, auto& b) { return a.tick < b.tick; });

	// handle edge cases for tempos
	if (tempos.empty())
	{
		tempos.emplace_back(0, 120.0);
	}
	else if (tempos.front().tick > 0)
	{
		tempos.insert(tempos.begin(), TempoEvent(0, 120.0));
	}

	if (timeSignatures.empty())
	{
		timeSignatures.emplace_back(0, 4, 4);
	}
	else if (timeSignatures.front().tick > 0)
	{
		timeSignatures.insert(timeSignatures.begin(), TimeSignatureEvent(0, 4, 4));
	}

	std::cout << "  Parsing finished! Merging events..." << std::endl;
	SetName("Parsing finished! Merging events...");

	NoteSequence mergedNotes = SequenceFuncs::FlattenSequence(std::move(notesToMerge));
	std::vector<MIDIMessageEvent> mergedEvents = SequenceFuncs::FlattenSequence(std::move(eventsToMerge));

	SetName("Finalizing (1/2)...");
	std::cout << "  Finishing up" << std::endl;
	seq->mergedNotes = SequenceFuncs::DistributeNotes(std::move(mergedNotes));
	seq->mergedEvents = std::move(mergedEvents);

	seq->tempoMap = std::make_shared<TempoMap>();
	seq->tempoMap->RebuildTempoMap(seq.get());

	// NEW: Apply note bounds
	ClearBars();

#pragma region Time-based loading if enabled
	if (timeBasedLoading)
	{
		std::cout << "Applying tempo events..." << std::endl;
		std::cout << "  Processing notes" << std::endl;
		TempoMap* tempoMap = seq->GetTempoMap();
		for (auto& notes : seq->mergedNotes)
		{
			SequenceFuncs::ApplyTempoEvents(seq->resolution, tempoMap, notes);
		}
		std::cout << "  Processing events" << std::endl;
		SequenceFuncs::ApplyTempoEvents(seq->resolution, tempoMap, seq->mergedEvents);
		SequenceFuncs::ApplyTempoEvents(seq->resolution, tempoMap, seq->timeSignatures);
	}
	seq->timeBased = timeBasedLoading;
#pragma endregion

	SetName("Finalizing (2/2)...\nIndexing note blocks...");
	size_t blockProgress = 0;
	AddBar([this, &blockProgress]() { return (double)blockProgress / (double)seq->notes; });

	std::array<std::vector<NoteBlock>, MIDI_KEYS> noteBlockArray{};

	size_t key = 0;
	for (const NoteSequence& notes : seq->mergedNotes)
	{
		std::vector<NoteBlock> blocks{};
		blocks.reserve((notes.Size() + NOTE_BLOCK_SIZE - 1) / NOTE_BLOCK_SIZE);

		uint32_t blockMin = (uint32_t)(-1); // funny underflow hack
		uint32_t blockMax = 0;

		for (size_t i = 0; i < notes.Size(); ++i)
		{
			uint32_t tick = notes.tick[i];
			uint32_t tickEnd = tick + notes.gate[i];

			blockMin = std::min(blockMin, tick);
			blockMax = std::max(blockMax, tickEnd);

			++blockProgress;

			if ((i + 1) % NOTE_BLOCK_SIZE == 0)
			{
				blocks.emplace_back(blockMin, blockMax);
				blockMin = (uint32_t)(-1);
				blockMax = 0;
			}
		}

		if (notes.Size() % NOTE_BLOCK_SIZE != 0)
		{
			blocks.emplace_back(blockMin, blockMax);
		}

		noteBlockArray[key++] = std::move(blocks);
	}
	seq->noteBlocks = std::move(noteBlockArray);

	ClearBars();

	SetName("Done!");
	std::cout << "MIDI has successfully loaded." << std::endl;
	std::cout << "  " << seq->tempos.size() << " Tempo events" << std::endl;
	std::cout << "  " << seq->timeSignatures.size() << " Time signature events" << std::endl;
	std::cout << "  Notes: " << seq->notes << std::endl;
	std::cout << "  Duration: " << seq->CalcLengthMilliseconds() << "ms" << std::endl;

	if (!illegalTracks.empty())
	{
		std::cout << "  Note events with different channel was mixed in Track ";
		for (int i = 0; i < illegalTracks.size(); i++)
		{
			if (i != 0)
			{
				std::cout << ((i == illegalTracks.size() - 1) ? " and " : ", ");
			}
			std::cout << illegalTracks[i];
		}
		std::cout << ". This means this MIDI can't be loaded in Domino :(" << std::endl;
	}

#pragma endregion
	return seq;
}

AbstractMIDILoader::ParsedTrack MultithreadedMIDILoader::ParseTrackData(const RawTrackChunk& chunk)
{
	ByteCursor cursor{ chunk.data, chunk.data + chunk.length };

	std::array<std::vector<size_t>, MIDI_KEYS << 4> unendedNotes{};
	size_t currNoteId = 0;

	uint32_t currTick = 0;
	uint8_t prevCommand = 0;
	bool ended = false;

	uint8_t firstChannel = 255;

	ParsedTrack midiTrack;

	// A dense track spends about six bytes per note (on and off). Reserving a fraction of the
	// chunk keeps the three note arrays from doubling over and over without over-committing.
	midiTrack.notes.Reserve(chunk.length / 12 + 16);

	while (running && !ended && !cursor.AtEnd())
	{
		currTick += cursor.VLQ();
		if (cursor.AtEnd()) break;

		uint8_t command = cursor.Byte();
		if (command < 0x80)
		{
			if (prevCommand == 0) continue; // stray data byte with no status to attach it to
			cursor.p--;
			command = prevCommand;
		}
		prevCommand = command;

		const uint8_t channel = command & 0x0F;

		if (!midiTrack.multiChannel && (command & 0xF0) >= 0x80 && (command & 0xF0) <= 0xE0)
		{
			if (firstChannel == 255)
			{
				firstChannel = channel;
			}
			else if (firstChannel != channel)
			{
				midiTrack.multiChannel = true;
			}
		}

		switch (command & 0xF0)
		{
			case 0x80:
			{
				if (cursor.Remaining() < 2) { ended = true; break; }
				const uint8_t key = cursor.Byte();
				cursor.Skip(1);

				if (key >= MIDI_KEYS) continue;

				auto& un = unendedNotes[((size_t)key << 4) | (size_t)channel];
				if (!un.empty())
				{
					const size_t n = un.back();
					un.pop_back();
					midiTrack.notes.gate[n] = currTick - midiTrack.notes.tick[n];
				}

				continue;
			}
			case 0x90:
			{
				if (cursor.Remaining() < 2) { ended = true; break; }
				const uint8_t key = cursor.Byte();
				const uint8_t vel = cursor.Byte();

				if (key >= MIDI_KEYS) continue;

				auto& un = unendedNotes[((size_t)key << 4) | (size_t)channel];

				if (vel > 0)
				{
					midiTrack.notes.Emplace(chunk.index, channel, currTick, key, (uint32_t)(-1), vel);
					un.push_back(currNoteId);
					currNoteId++;
				}
				else if (!un.empty())
				{
					const size_t n = un.back();
					un.pop_back();
					midiTrack.notes.gate[n] = currTick - midiTrack.notes.tick[n];
				}
				continue;
			}
			case 0xA0:
			case 0xB0:
			case 0xE0:
			{
				if (cursor.Remaining() < 2) { ended = true; break; }
				const uint8_t data1 = cursor.Byte();
				const uint8_t data2 = cursor.Byte();

				if (loadOnlyNotes) continue;

				midiTrack.messages.emplace_back(
					currTick,
					((uint32_t)data2 << 16) | ((uint32_t)data1 << 8) | (uint32_t)command
				);
				continue;
			}

			case 0xC0:
			case 0xD0:
			{
				if (cursor.AtEnd()) { ended = true; break; }
				const uint8_t data1 = cursor.Byte();

				if (loadOnlyNotes) continue;

				midiTrack.messages.emplace_back(
					currTick,
					((uint32_t)data1 << 8) | (uint32_t)command
				);
				continue;
			}
			case 0xF0:
			{
				if (command == 0xFF)
				{
					if (cursor.AtEnd()) { ended = true; break; }
					const uint8_t type = cursor.Byte();
					const uint32_t len = cursor.VLQ();

					switch (type)
					{
						case 0x2F:
							ended = true;
							continue;
						case 0x51:
							if (len == 3 && cursor.Remaining() >= 3)
							{
								const long msec = ((long)cursor.Byte() << 16) | ((long)cursor.Byte() << 8) | (long)cursor.Byte();
								// A zero tempo is how damaged files stop time; ignore it instead of
								// dropping the rest of the track.
								if (msec > 0) midiTrack.tempos.emplace_back(currTick, 6.0e7 / (double)msec);
							}
							else
							{
								cursor.Skip(len);
							}
							continue;
						case 0x58:
							if (len == 4 && cursor.Remaining() >= 4)
							{
								const uint8_t numerator = cursor.Byte();
								const uint8_t denominator = (uint8_t)(1 << cursor.Byte());
								cursor.Skip(2);
								midiTrack.timeSignatures.emplace_back(currTick, numerator, denominator);
							}
							else
							{
								cursor.Skip(len);
							}
							continue;
						case 0x03:
						{
							// Track name, shown by the per-track multiplier settings.
							uint8_t text[128];
							const uint32_t read = std::min<uint32_t>(len, (uint32_t)sizeof(text));
							for (uint32_t b = 0; b < read; b++) text[b] = cursor.Byte();
							midiTrack.name = SanitizeTrackName(text, read);
							cursor.Skip(len - read);
							continue;
						}
						case 0x0A:
							cursor.Skip(len);
							continue;
						default:
							cursor.Skip(len);
							continue;
					}
				}

				if (command == 0xF0 || command == 0xF7)
				{
					const uint32_t len = cursor.VLQ();
					cursor.Skip(len);
					continue;
				}

				if (command == 0xF2)
				{
					cursor.Skip(2);
					continue;
				}

				if (command == 0xF3)
				{
					cursor.Skip(1);
					continue;
				}

				continue;
			}
			default:
				continue;
		}
	}

	midiTrack.length = currTick;
	return midiTrack;
}
