#include "ArchiveLoader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <archive.h>
#include <archive_entry.h>

namespace
{
	// A decompression bomb should fail loudly instead of taking the machine down with it.
	constexpr uint64_t MAX_EXTRACTED_BYTES = 8ull * 1024ull * 1024ull * 1024ull;
	constexpr int MAX_NESTING = 8;

	const uint8_t SIG_ZIP[] = { 'P', 'K', 0x03, 0x04 };
	const uint8_t SIG_7Z[] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
	const uint8_t SIG_XZ[] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };
	const uint8_t SIG_GZ[] = { 0x1F, 0x8B };
	const uint8_t SIG_BZ2[] = { 'B', 'Z', 'h' };
	const uint8_t SIG_ZSTD[] = { 0x28, 0xB5, 0x2F, 0xFD };
	const uint8_t SIG_LZ4[] = { 0x04, 0x22, 0x4D, 0x18 };
	const uint8_t SIG_RAR[] = { 'R', 'a', 'r', '!', 0x1A, 0x07 };
	const uint8_t SIG_LZMA[] = { 0x5D, 0x00, 0x00 };

	bool StartsWith(const uint8_t* data, size_t size, const uint8_t* sig, size_t sigSize)
	{
		return size >= sigSize && std::memcmp(data, sig, sigSize) == 0;
	}

	bool IsMidiHeader(const uint8_t* data, size_t size)
	{
		return size >= 4 && std::memcmp(data, "MThd", 4) == 0;
	}

	bool LooksCompressed(const uint8_t* data, size_t size)
	{
		return StartsWith(data, size, SIG_ZIP, sizeof(SIG_ZIP))
			|| StartsWith(data, size, SIG_7Z, sizeof(SIG_7Z))
			|| StartsWith(data, size, SIG_XZ, sizeof(SIG_XZ))
			|| StartsWith(data, size, SIG_GZ, sizeof(SIG_GZ))
			|| StartsWith(data, size, SIG_BZ2, sizeof(SIG_BZ2))
			|| StartsWith(data, size, SIG_ZSTD, sizeof(SIG_ZSTD))
			|| StartsWith(data, size, SIG_LZ4, sizeof(SIG_LZ4))
			|| StartsWith(data, size, SIG_RAR, sizeof(SIG_RAR))
			|| StartsWith(data, size, SIG_LZMA, sizeof(SIG_LZMA));
	}

	bool EndsWithIgnoreCase(const std::string& text, const char* suffix)
	{
		const size_t suffixSize = std::strlen(suffix);
		if (text.size() < suffixSize) return false;

		for (size_t i = 0; i < suffixSize; i++)
		{
			const char a = (char)std::tolower((unsigned char)text[text.size() - suffixSize + i]);
			const char b = (char)std::tolower((unsigned char)suffix[i]);
			if (a != b) return false;
		}
		return true;
	}

	const char* const ARCHIVE_EXTENSIONS[] = {
		".7z", ".xz", ".lzma", ".zip", ".gz", ".bz2", ".bzip2", ".tar",
		".zst", ".zstd", ".lz4", ".rar", ".tgz", ".tbz2", ".tbz", ".txz", ".cab"
	};

	bool IsArchiveExtension(const std::string& name)
	{
		for (const char* extension : ARCHIVE_EXTENSIONS)
		{
			if (EndsWithIgnoreCase(name, extension)) return true;
		}
		return false;
	}

	// Keeps an inner .mid through its wrappers: song.mid.xz is still a MIDI.
	bool NameSuggestsMidi(const std::string& name)
	{
		if (EndsWithIgnoreCase(name, ".mid") || EndsWithIgnoreCase(name, ".midi")) return true;

		for (const char* extension : ARCHIVE_EXTENSIONS)
		{
			if (EndsWithIgnoreCase(name, extension))
			{
				return NameSuggestsMidi(name.substr(0, name.size() - std::strlen(extension)));
			}
		}
		return false;
	}

	struct Candidate
	{
		std::vector<uint8_t> data;
		std::string name;
		int64_t declaredSize = -1;
		bool nested = false;
	};

	bool ReadEntryData(archive* a, const uint8_t* prefix, size_t prefixSize, int64_t declaredSize,
		const std::atomic<bool>* cancel, std::vector<uint8_t>& out)
	{
		out.clear();
		if (declaredSize > 0 && (uint64_t)declaredSize <= MAX_EXTRACTED_BYTES)
		{
			out.reserve((size_t)declaredSize);
		}
		out.insert(out.end(), prefix, prefix + prefixSize);

		uint8_t buffer[256 * 1024];
		for (;;)
		{
			if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return false;

			const la_ssize_t got = archive_read_data(a, buffer, sizeof(buffer));
			if (got == 0) break;
			if (got < 0) return false;

			if (out.size() + (size_t)got > MAX_EXTRACTED_BYTES) return false;
			out.insert(out.end(), buffer, buffer + got);
		}

		return true;
	}

	// Walks the entries of an already-open archive and leaves the best MIDI candidate in `best`.
	bool FindBestEntry(archive* a, const std::atomic<bool>* cancel,
		std::atomic<uint64_t>* done, Candidate& best, std::string& error)
	{
		archive_entry* entry = nullptr;

		while (true)
		{
			if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
			{
				error = "Loading canceled.";
				return false;
			}

			const int headerResult = archive_read_next_header(a, &entry);
			if (headerResult == ARCHIVE_EOF) break;
			if (headerResult < ARCHIVE_WARN)
			{
				error = archive_error_string(a) != nullptr ? archive_error_string(a) : "Could not read the archive.";
				return false;
			}

			if (archive_entry_filetype(entry) == AE_IFDIR)
			{
				archive_read_data_skip(a);
				continue;
			}

			const char* rawName = archive_entry_pathname_utf8(entry);
			if (rawName == nullptr) rawName = archive_entry_pathname(entry);
			const std::string name = rawName != nullptr ? rawName : std::string();

			const bool sizeKnown = archive_entry_size_is_set(entry) != 0;
			const int64_t declaredSize = sizeKnown ? archive_entry_size(entry) : -1;

			// Peek at the first bytes: the entry may be a MIDI with no useful name, or a wrapper
			// around another compressed layer.
			uint8_t header[16] = {};
			size_t headerSize = 0;
			while (headerSize < sizeof(header))
			{
				const la_ssize_t got = archive_read_data(a, header + headerSize, sizeof(header) - headerSize);
				if (got <= 0) break;
				headerSize += (size_t)got;
			}

			const bool midi = IsMidiHeader(header, headerSize);
			const bool nested = !midi && LooksCompressed(header, headerSize);
			const bool named = NameSuggestsMidi(name);

			if (!midi && !nested && !named)
			{
				archive_read_data_skip(a);
				continue;
			}

			// Nothing is gained by decompressing an entry that cannot beat what we already have.
			if (!nested && declaredSize >= 0 && best.declaredSize >= 0 && !best.data.empty()
				&& declaredSize <= best.declaredSize)
			{
				archive_read_data_skip(a);
				continue;
			}

			Candidate candidate;
			candidate.name = name;
			candidate.declaredSize = declaredSize;
			candidate.nested = nested;

			if (!ReadEntryData(a, header, headerSize, declaredSize, cancel, candidate.data))
			{
				if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
				{
					error = "Loading canceled.";
					return false;
				}
				continue;
			}

			const bool candidateMidi = IsMidiHeader(candidate.data.data(), candidate.data.size());
			if (!candidateMidi && !LooksCompressed(candidate.data.data(), candidate.data.size()))
			{
				continue;
			}

			const bool bestMidi = IsMidiHeader(best.data.data(), best.data.size());
			bool better;
			if (best.data.empty()) better = true;
			else if (candidateMidi != bestMidi) better = candidateMidi;
			else if (candidate.declaredSize >= 0 && best.declaredSize >= 0)
				better = candidate.declaredSize > best.declaredSize;
			else
				better = candidate.data.size() > best.data.size();

			if (better) best = std::move(candidate);

			if (done != nullptr)
			{
				const la_int64_t consumed = archive_filter_bytes(a, -1);
				done->store(consumed > 0 ? (uint64_t)consumed : 0, std::memory_order_relaxed);
			}
		}

		return true;
	}

	ArchiveLoader::Result ExtractFromArchive(archive* a, const std::atomic<bool>* cancel,
		std::atomic<uint64_t>* done)
	{
		ArchiveLoader::Result result;

		Candidate best;
		std::string error;

		if (!FindBestEntry(a, cancel, done, best, error))
		{
			result.error = error;
			return result;
		}

		if (best.data.empty())
		{
			result.error = "No MIDI file was found inside this archive.";
			return result;
		}

		// Follow wrappers (a MIDI cut into a .xz, then wrapped in a .7z, ...).
		for (int depth = 0; depth < MAX_NESTING; depth++)
		{
			if (IsMidiHeader(best.data.data(), best.data.size()))
			{
				result.ok = true;
				result.data = std::move(best.data);
				result.entryName = best.name;
				return result;
			}

			if (!LooksCompressed(best.data.data(), best.data.size()))
			{
				break;
			}

			archive* inner = archive_read_new();
			if (inner == nullptr)
			{
				break;
			}

			archive_read_support_filter_all(inner);
			archive_read_support_format_all(inner);
			archive_read_support_format_raw(inner);

			if (archive_read_open_memory(inner, best.data.data(), best.data.size()) != ARCHIVE_OK)
			{
				result.error = archive_error_string(inner) != nullptr ? archive_error_string(inner) : "Could not open the compressed data.";
				archive_read_free(inner);
				return result;
			}

			ArchiveLoader::Result next = ExtractFromArchive(inner, cancel, nullptr);
			archive_read_free(inner);

			if (!next.ok)
			{
				result.error = next.error;
				return result;
			}

			best.data = std::move(next.data);
			best.name = next.entryName;
		}

		result.error = "No MIDI file was found inside this archive.";
		return result;
	}
}

bool ArchiveLoader::IsArchive(const std::filesystem::path& path)
{
	const std::string name = path.filename().string();
	if (IsArchiveExtension(name)) return true;

	// Extensionless downloads are common; let the first bytes decide.
	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open()) return false;

	uint8_t header[16] = {};
	stream.read(reinterpret_cast<char*>(header), sizeof(header));
	const size_t read = (size_t)stream.gcount();

	if (IsMidiHeader(header, read)) return false;
	return LooksCompressed(header, read);
}

ArchiveLoader::Result ArchiveLoader::Extract(const std::filesystem::path& path,
	const std::atomic<bool>* cancel,
	std::atomic<uint64_t>* done,
	std::atomic<uint64_t>* total)
{
	Result result;

	std::error_code sizeError;
	const uint64_t fileSize = std::filesystem::file_size(path, sizeError);
	if (total != nullptr) total->store(sizeError ? 0 : fileSize, std::memory_order_relaxed);

	archive* a = archive_read_new();
	if (a == nullptr)
	{
		result.error = "Could not initialize the archive reader.";
		return result;
	}

	archive_read_support_filter_all(a);
	archive_read_support_format_all(a);
	archive_read_support_format_raw(a);

#ifdef _WIN32
	const int openResult = archive_read_open_filename_w(a, path.wstring().c_str(), 1 << 20);
#else
	const int openResult = archive_read_open_filename(a, path.string().c_str(), 1 << 20);
#endif

	if (openResult != ARCHIVE_OK)
	{
		const char* message = archive_error_string(a);
		result.error = message != nullptr ? message : "Could not open the file as an archive.";
		archive_read_free(a);
		return result;
	}

	result = ExtractFromArchive(a, cancel, done);

	// The reader consumed the outer file, so the bar is full even when we cancelled.
	if (done != nullptr)
	{
		done->store(fileSize, std::memory_order_relaxed);
	}

	archive_read_free(a);
	return result;
}
