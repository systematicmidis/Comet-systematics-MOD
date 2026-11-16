#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <lzma.h>

#include "MIDI/MultithreadedMIDILoader.h"

namespace
{
	constexpr uint64_t HUGE_NOTES = 270'000'000; // 1.08 GB of note-ons, above the old 1 GB stop
	constexpr uint32_t TAIL_NOTES = 3;

	class XzWriter
	{
	public:
		explicit XzWriter(const std::filesystem::path& path)
		{
			file = std::fopen(path.string().c_str(), "wb");
			if (file != nullptr)
				ok = lzma_easy_encoder(&stream, 0, LZMA_CHECK_CRC32) == LZMA_OK;
		}

		~XzWriter()
		{
			if (file != nullptr) std::fclose(file);
			lzma_end(&stream);
		}

		bool Ok() const { return ok && file != nullptr; }

		bool Write(const void* data, size_t size)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			stream.next_in = bytes;
			stream.avail_in = size;

			std::vector<uint8_t> output(1 << 20);
			while (stream.avail_in > 0)
			{
				stream.next_out = output.data();
				stream.avail_out = output.size();

				const lzma_ret result = lzma_code(&stream, LZMA_RUN);
				if (result != LZMA_OK) return ok = false;

				const size_t produced = output.size() - stream.avail_out;
				if (produced > 0 && std::fwrite(output.data(), 1, produced, file) != produced)
					return ok = false;
			}

			return ok;
		}

		bool Finish()
		{
			std::vector<uint8_t> output(1 << 20);
			for (;;)
			{
				stream.next_out = output.data();
				stream.avail_out = output.size();

				const lzma_ret result = lzma_code(&stream, LZMA_FINISH);
				if (result != LZMA_OK && result != LZMA_STREAM_END) return ok = false;

				const size_t produced = output.size() - stream.avail_out;
				if (produced > 0 && std::fwrite(output.data(), 1, produced, file) != produced)
					return ok = false;

				if (result == LZMA_STREAM_END) return true;
			}
		}

	private:
		std::FILE* file = nullptr;
		lzma_stream stream = LZMA_STREAM_INIT;
		bool ok = false;
	};

	void WriteBigEndian32(uint8_t* out, uint32_t value)
	{
		out[0] = (uint8_t)(value >> 24);
		out[1] = (uint8_t)(value >> 16);
		out[2] = (uint8_t)(value >> 8);
		out[3] = (uint8_t)value;
	}

	bool WriteHugeTrack(XzWriter& writer)
	{
		constexpr uint8_t NOTE_ON[4] = { 0x00, 0x90, 0x40, 0x64 };
		constexpr uint8_t NOTE_OFF[4] = { 0x00, 0x80, 0x40, 0x00 };
		constexpr uint8_t END[4] = { 0x00, 0xFF, 0x2F, 0x00 };

		const uint64_t noteBytes = HUGE_NOTES * sizeof(NOTE_ON);
		const uint32_t trackLength = (uint32_t)(noteBytes + sizeof(NOTE_OFF) + sizeof(END));

		uint8_t header[8] = { 'M', 'T', 'r', 'k', 0, 0, 0, 0 };
		WriteBigEndian32(header + 4, trackLength);
		if (!writer.Write(header, sizeof(header))) return false;

		std::vector<uint8_t> chunk(1 << 20);
		for (size_t i = 0; i < chunk.size(); i += sizeof(NOTE_ON))
			std::memcpy(chunk.data() + i, NOTE_ON, sizeof(NOTE_ON));

		uint64_t remaining = noteBytes;
		while (remaining > 0)
		{
			const size_t amount = (size_t)std::min<uint64_t>(remaining, chunk.size());
			if (!writer.Write(chunk.data(), amount)) return false;
			remaining -= amount;
		}

		if (!writer.Write(NOTE_OFF, sizeof(NOTE_OFF))) return false;
		if (!writer.Write(END, sizeof(END))) return false;

		return true;
	}

	bool WriteTailTrack(XzWriter& writer)
	{
		const uint8_t tail[] = {
			0x00, 0x90, 0x30, 0x64, 0x00, 0x80, 0x30, 0x00,
			0x00, 0x90, 0x34, 0x64, 0x00, 0x80, 0x34, 0x00,
			0x00, 0x90, 0x37, 0x64, 0x00, 0x80, 0x37, 0x00,
			0x00, 0xFF, 0x2F, 0x00
		};

		uint8_t header[8] = { 'M', 'T', 'r', 'k', 0, 0, 0, 0 };
		WriteBigEndian32(header + 4, sizeof(tail));
		return writer.Write(header, sizeof(header)) && writer.Write(tail, sizeof(tail));
	}

	bool BuildArchive(const std::filesystem::path& path)
	{
		XzWriter writer(path);
		if (!writer.Ok()) return false;

		const uint8_t header[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 4, 0, 0x60 };
		const uint8_t tempo[] = {
			'M', 'T', 'r', 'k', 0, 0, 0, 11,
			0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
			0x00, 0xFF, 0x2F, 0x00
		};

		return writer.Write(header, sizeof(header))
			&& writer.Write(tempo, sizeof(tempo))
			&& WriteHugeTrack(writer)
			&& WriteHugeTrack(writer)
			&& WriteTailTrack(writer)
			&& writer.Finish();
	}

	struct TempFile
	{
		std::filesystem::path path;
		~TempFile()
		{
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	};

	bool BuildSmallLegacyMidi(const std::filesystem::path& path)
	{
		const uint8_t header[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 7, 0, 0x60 };
		const uint8_t tempo[] = {
			0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
			0x00, 0xFF, 0x2F, 0x00
		};
		const uint8_t empty[] = {
			0x00, 0xFF, 0x03, 0x04, 'p', 'a', 'd', ' ',
			0x00, 0xFF, 0x2F, 0x00
		};

		std::vector<uint8_t> bytes(header, header + sizeof(header));
		auto addTrack = [&](const uint8_t* data, uint32_t size)
		{
			const uint8_t chunk[] = {
				'M', 'T', 'r', 'k',
				(uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size
			};
			bytes.insert(bytes.end(), chunk, chunk + sizeof(chunk));
			bytes.insert(bytes.end(), data, data + size);
		};
		auto addNoteTrack = [&](uint8_t key)
		{
			const uint8_t note[] = {
				0x00, 0x90, key, 0x64,
				0x60, 0x80, key, 0x00,
				0x00, 0xFF, 0x2F, 0x00
			};
			addTrack(note, sizeof(note));
		};

		addTrack(tempo, sizeof(tempo));
		addNoteTrack(60);
		addNoteTrack(60); // duplicate of the first note track
		addTrack(empty, sizeof(empty));
		addNoteTrack(64);
		addNoteTrack(64); // duplicate of the second note track
		addNoteTrack(67);

		std::ofstream file(path, std::ios::binary);
		if (!file) return false;
		file.write((const char*)bytes.data(), (std::streamsize)bytes.size());
		return (bool)file;
	}

	bool CheckLegacyMapping(const std::shared_ptr<MIDISequence>& sequence)
	{
		if (!sequence || sequence->noteTrackCount != 5) return false;
		if (sequence->trackCopies.size() != 3
			|| sequence->trackCopies[0] != 2
			|| sequence->trackCopies[1] != 2
			|| sequence->trackCopies[2] != 1) return false;
		if (sequence->noteTrackStoredIndex.size() != 5) return false;
		const int expected[] = { 0, 0, 1, 1, 2 };
		for (size_t i = 0; i < sequence->noteTrackStoredIndex.size(); i++)
			if (sequence->noteTrackStoredIndex[i] != expected[i]) return false;
		return !sequence->mergedNotes.empty();
	}
}

int main()
{
	TempFile temp{ std::filesystem::temp_directory_path() / "comet_archive_repeat_regression.mid.xz" };

	std::cout << "Building " << temp.path.string() << std::endl;
	if (!BuildArchive(temp.path))
	{
		std::cerr << "FAIL: could not build the test archive" << std::endl;
		return 1;
	}

	MultithreadedMIDILoader loader(temp.path.string().c_str());
	// The old early stop only ran with this option on, as the app's default config did.
	loader.SetLayeredDedupe(true, true);

	const auto sequence = loader.Load();
	const long long expected = (long long)HUGE_NOTES * 2 + TAIL_NOTES;

	std::cout << "Expected notes: " << expected << std::endl;
	if (!sequence)
	{
		std::cerr << "FAIL: loader returned no sequence" << std::endl;
		return 1;
	}

	std::cout << "Actual notes:   " << sequence->notes << std::endl;
	if (sequence->notes != expected)
	{
		std::cerr << "FAIL: a tail track was dropped or a repeat was undercounted" << std::endl;
		return 1;
	}

	if (sequence->trackCount != 4)
	{
		std::cerr << "FAIL: expected 4 tracks, got " << sequence->trackCount << std::endl;
		return 1;
	}

	TempFile legacyTemp{ std::filesystem::temp_directory_path() / "comet_legacy_track_mapping.mid" };
	if (!BuildSmallLegacyMidi(legacyTemp.path))
	{
		std::cerr << "FAIL: could not build the legacy mapping file" << std::endl;
		return 1;
	}

	MultithreadedMIDILoader legacyLoader(legacyTemp.path.string().c_str());
	legacyLoader.SetLayeredDedupe(true, true);
	const auto legacy = legacyLoader.Load();
	if (!CheckLegacyMapping(legacy))
	{
		std::cerr << "FAIL: note-track indices do not map to the stored repeated tracks" << std::endl;
		return 1;
	}

	std::cout << "PASS: two huge repeats counted, tail kept, and legacy repeat weights mapped" << std::endl;
	return 0;
}
