#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// A forward-only byte stream of decompressed data. Used for nested .xz files that are far too
// large to hold in memory, so the MIDI can be parsed track by track while it is still compressed.
class ArchiveStream
{
public:
	virtual ~ArchiveStream() = default;

	// Fills up to max bytes and returns how many were produced; 0 means the stream ended.
	virtual size_t Read(uint8_t* out, size_t max) = 0;

	// False when the decoder hit damaged or truncated input.
	virtual bool Ok() const { return true; }

	// Bytes of the outermost compressed file consumed so far, for the progress bar.
	virtual uint64_t CompressedRead() const = 0;
	virtual uint64_t CompressedTotal() const = 0;
};

// Reads the MIDI out of a compressed file or archive (7z, xz, lzma, zip, gz, bz2, zst, lz4, rar,
// tar, and compressed tars), including nested layers such as a .mid.xz inside a .7z. Container
// entries are streamed when possible; anything nested or unnamed is decompressed into memory.
// Nothing is written to disk.
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

	struct EntryInfo
	{
		std::string name;
		uint64_t size = 0;
	};

	// Lists named MIDI entries without decompressing them. This is what the entry picker uses
	// when an archive contains more than one song. Returns an empty list for ordinary files.
	static std::vector<EntryInfo> ListMidiEntries(const std::filesystem::path& path,
		std::string* error = nullptr);

	// Decompresses the MIDI entry to memory. When the archive holds several, the largest MIDI wins;
	// when an entry is itself compressed, the layers are followed. done/total track the outermost
	// file so the loading dialog can show a progress bar, and cancel is polled while reading.
	static Result Extract(const std::filesystem::path& path,
		const std::atomic<bool>* cancel,
		std::atomic<uint64_t>* done = nullptr,
		std::atomic<uint64_t>* total = nullptr,
		const std::string& preferredEntry = {});

	// Bytes of decompressed data the in-memory extractor will accept on this machine. Streamed
	// entries use it as the note-store ceiling too, since notes are what they still keep.
	static uint64_t MemoryBudget();

	// Risky fast dedup: stop decoding a layer once its first tile has repeated and the rest of the
	// layer divides exactly by that tile's size. Wrong on a layer made of different tiles that
	// happen to fit, which is why it is off unless the user turns it on for a session.
	static void SetRiskyFastDedupe(bool on);

	// Opens a compressed file as a stream, so a MIDI that expands to more than this machine can
	// hold is parsed as it is produced. Pure xz chains are decoded directly; for 7z, zip, rar, tar
	// and similar containers the selected MIDI entry is streamed out of libarchive. Returns
	// nullptr when the file is not streamable (a nested container or an unnamed entry), and
	// callers then fall back to Extract(). Nothing is ever written to disk.
	static std::unique_ptr<ArchiveStream> OpenStream(const std::filesystem::path& path,
		std::string& error, const std::string& preferredEntry = {});

	// A "header + N identical compressed cycles" archive can be loaded without decoding all N
	// cycles: this proves every byte of the repeated region matches the first cycle, then returns
	// a stream holding the MIDI header plus that one cycle. The caller parses it once and
	// multiplies the notes by `cycles`. Returns nullptr whenever the proof does not hold, so the
	// caller can fall back to the complete walk.
	struct RepeatedCycleInfo
	{
		uint64_t cycles = 0;
		uint16_t declaredTracks = 0;
		std::vector<uint8_t> preambleMidi;
	};
	static std::unique_ptr<ArchiveStream> OpenProvenRepeatStream(const std::filesystem::path& path,
		RepeatedCycleInfo& info, std::string& error);

	// A read-only stream over bytes already in memory (used for the proven preamble).
	static std::unique_ptr<ArchiveStream> OpenMemoryStream(const uint8_t* data, size_t size);
};
