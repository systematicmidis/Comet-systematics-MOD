#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Reads the MIDI out of a compressed file or archive (7z, xz, lzma, zip, gz, bz2, zst, lz4, rar,
// tar, and compressed tars), including nested layers such as a .mid.xz inside a .7z. Everything is
// decompressed into memory; nothing is written to disk.
class ArchiveLoader
{
public:
	// True when the path is a compressed file/archive. The extension decides first; a file with no
	// useful extension is recognised from its first bytes.
	static bool IsArchive(const std::filesystem::path& path);

	struct Result
	{
		bool ok = false;
		std::vector<uint8_t> data;
		std::string entryName; // the entry the MIDI came from, for the log
		std::string error;
	};

	// Decompresses the MIDI entry to memory. When the archive holds several, the largest MIDI wins;
	// when an entry is itself compressed, the layers are followed. done/total track the outermost
	// file so the loading dialog can show a progress bar, and cancel is polled while reading.
	static Result Extract(const std::filesystem::path& path,
		const std::atomic<bool>* cancel,
		std::atomic<uint64_t>* done = nullptr,
		std::atomic<uint64_t>* total = nullptr);

	// Risky fast dedup: stop decoding a layer once its first tile has repeated and the rest of the
	// layer divides exactly by that tile's size. Wrong on a layer made of different tiles that
	// happen to fit, which is why it is off unless the user turns it on for a session.
	static void SetRiskyFastDedupe(bool on);
};
