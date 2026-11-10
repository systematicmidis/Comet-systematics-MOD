#pragma once

#include "AbstractMIDILoader.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "MIDISequence.h"
#include "IO/MappedFile.h"

// Parses every track in parallel straight from the file's memory mapping (or from the bytes of a
// decompressed archive). There is no shared file position, no per-track buffer and no lock.
class MultithreadedMIDILoader : public AbstractMIDILoader
{
public:
	MultithreadedMIDILoader(const char* file);
	MultithreadedMIDILoader(std::shared_ptr<InputStream> is);
	~MultithreadedMIDILoader()
	{
		Stop();
		seq.reset();
	}

	std::shared_ptr<MIDISequence> Load(bool timeBasedLoading = false) override;

	// 0 leaves the worker count to the machine; 1 parses on this thread.
	void SetWorkerLimit(size_t workers) { workerLimit = workers; }

	void Stop() override
	{
		AbstractMIDILoader::Stop();
		running = false;
	}

private:
	struct RawTrackChunk
	{
		const uint8_t* data = nullptr;
		size_t length = 0;
		size_t index = 0;
	};

	// isolated worker task to optimize cache usage and eliminate synchronization locks
	ParsedTrack ParseTrackData(const RawTrackChunk& chunk);

	std::string file;
	std::shared_ptr<MIDISequence> seq;
	std::shared_ptr<InputStream> is;

	std::atomic<bool> running{ false };
	std::atomic<size_t> tracksProcessed{ 0 };
	size_t workerLimit = 0;

	// Progress of the decompression phase, read by the loading dialog's bar.
	std::atomic<uint64_t> archiveDone{ 0 };
	std::atomic<uint64_t> archiveTotal{ 0 };

	// Keep the mapped view / decompressed bytes alive for as long as the loader is.
	std::unique_ptr<MappedFile> mappedFile;
	std::vector<uint8_t> ownedBytes;
};
