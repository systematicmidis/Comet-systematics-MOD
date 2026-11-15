#include "ArchiveLoader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <archive.h>
#include <archive_entry.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>

#include <lzma.h>

namespace
{
	// A forward-only file reader; the root of a nested decompression chain.
	class FileStream final : public ArchiveStream
	{
	public:
		explicit FileStream(const std::filesystem::path& path)
		{
#ifdef _WIN32
			file = _wfopen(path.wstring().c_str(), L"rb");
#else
			file = std::fopen(path.string().c_str(), "rb");
#endif
			std::error_code ec;
			size = std::filesystem::file_size(path, ec);
			if (ec) size = 0;
		}

		~FileStream() override
		{
			if (file) std::fclose(file);
		}

		bool Ok() const { return file != nullptr; }

		size_t Read(uint8_t* out, size_t max) override
		{
			if (!file || max == 0) return 0;
			const size_t got = std::fread(out, 1, max, file);
			read += got;
			return got;
		}

		uint64_t CompressedRead() const override { return read; }
		uint64_t CompressedTotal() const override { return size; }

	private:
		std::FILE* file = nullptr;
		uint64_t read = 0;
		uint64_t size = 0;
	};

	// A read-only view over bytes that are already in memory.
	class BufferStream final : public ArchiveStream
	{
	public:
		explicit BufferStream(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}

		size_t Read(uint8_t* out, size_t max) override
		{
			if (pos >= data.size() || max == 0) return 0;
			const size_t got = std::min(max, data.size() - pos);
			std::memcpy(out, data.data() + pos, got);
			pos += got;
			return got;
		}

		uint64_t CompressedRead() const override { return pos; }
		uint64_t CompressedTotal() const override { return data.size(); }

	private:
		std::vector<uint8_t> data;
		size_t pos = 0;
	};

	// One .xz/.lzma layer. liblzma decodes concatenated streams too, which is how a "layer" can be
	// a long run of repeated xz streams.
	class XzStream final : public ArchiveStream
	{
	public:
		explicit XzStream(std::unique_ptr<ArchiveStream> input) : in(std::move(input))
		{
			lzma_stream blank = LZMA_STREAM_INIT;
			strm = blank;

			lzma_mt options{};
			options.flags = LZMA_CONCATENATED;
			options.threads = (uint32_t)std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
			options.block_size = 0;
			options.timeout = 0;
			options.memlimit_threading = 2ull << 30;
			options.memlimit_stop = UINT64_MAX;

			ok = lzma_stream_decoder_mt(&strm, &options) == LZMA_OK;
		}

		~XzStream() override { lzma_end(&strm); }

		bool Ok() const override { return ok; }

		size_t Read(uint8_t* out, size_t max) override
		{
			if (!ok || done || max == 0) return 0;

			strm.next_out = out;
			strm.avail_out = max;

			while (strm.avail_out > 0 && !done)
			{
				if (strm.avail_in == 0)
				{
					const size_t got = in ? in->Read(buffer, sizeof(buffer)) : 0;
					strm.next_in = buffer;
					strm.avail_in = got;

					if (got == 0)
					{
						// Tell the decoder the input is finished so it can flush and verify.
						const lzma_ret ret = lzma_code(&strm, LZMA_FINISH);
						done = true;
						if (ret != LZMA_STREAM_END) ok = false;
						break;
					}
				}

				const lzma_ret ret = lzma_code(&strm, LZMA_RUN);
				if (ret == LZMA_STREAM_END)
				{
					done = true;
					break;
				}
				if (ret != LZMA_OK)
				{
					ok = false;
					done = true;
					break;
				}
			}

			return max - strm.avail_out;
		}

		uint64_t CompressedRead() const override { return in ? in->CompressedRead() : 0; }
		uint64_t CompressedTotal() const override { return in ? in->CompressedTotal() : 0; }

	private:
		std::unique_ptr<ArchiveStream> in;
		lzma_stream strm = LZMA_STREAM_INIT;
		uint8_t buffer[1 << 20]{};
		bool ok = false;
		bool done = false;
	};

	// Puts the bytes read while probing the layer back in front of the stream.
	class PrefixedStream final : public ArchiveStream
	{
	public:
		PrefixedStream(std::unique_ptr<ArchiveStream> input, const uint8_t* data, size_t size)
			: in(std::move(input)), prefix(data, data + size)
		{
		}

		size_t Read(uint8_t* out, size_t max) override
		{
			size_t copied = 0;
			if (pos < prefix.size())
			{
				copied = std::min(max, prefix.size() - pos);
				std::memcpy(out, prefix.data() + pos, copied);
				pos += copied;
				if (copied == max) return copied;
			}

			if (!in) return copied;
			return copied + in->Read(out + copied, max - copied);
		}

		uint64_t CompressedRead() const override { return in ? in->CompressedRead() : 0; }
		uint64_t CompressedTotal() const override { return in ? in->CompressedTotal() : 0; }

	private:
		std::unique_ptr<ArchiveStream> in;
		std::vector<uint8_t> prefix;
		size_t pos = 0;
	};

	// Nested archives are decompressed layer by layer in memory, so the ceiling is whatever this
	// machine can actually hold. A fixed limit either refuses files that would have fit or invites an
	// out-of-memory kill; the headroom keeps the app and the finished MIDI alive too.
	uint64_t MemoryBudget()
	{
#ifdef _WIN32
		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);
		if (GlobalMemoryStatusEx(&status))
		{
			const uint64_t usable = (uint64_t)status.ullAvailPhys / 4ull * 3ull;
			return std::clamp<uint64_t>(usable, 1ull << 30, 32ull << 30);
		}
#endif
		return 8ull * 1024ull * 1024ull * 1024ull;
	}

	std::string HumanBytes(uint64_t bytes)
	{
		char buffer[64];
		const double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
		if (gb >= 1.0)
			std::snprintf(buffer, sizeof(buffer), "%.1f GB", gb);
		else
			std::snprintf(buffer, sizeof(buffer), "%.0f MB", (double)bytes / (1024.0 * 1024.0));
		return buffer;
	}

	// A decompression bomb should fail loudly instead of taking the machine down with it.
	constexpr int MAX_NESTING = 8;

	// Risky fast dedup, off until the user asks for it for this session.
	std::atomic<bool> g_riskyFastDedupe{ false };

	// The smallest p such that the buffer is the same p bytes over and over, or 0 when it is not an
	// exact repetition. This is the tile size the layer is built from.
	size_t SmallestPeriod(const uint8_t* data, size_t size)
	{
		if (size < 2) return 0;

		std::vector<size_t> prefix(size, 0);
		for (size_t i = 1, k = 0; i < size; i++)
		{
			while (k > 0 && data[i] != data[k]) k = prefix[k - 1];
			if (data[i] == data[k]) k++;
			prefix[i] = k;
		}

		const size_t period = size - prefix[size - 1];
		return (period > 0 && size % period == 0) ? period : 0;
	}

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

	bool ReadAll(ArchiveStream& stream, std::vector<uint8_t>& out, size_t limit, std::string& error)
	{
		std::vector<uint8_t> buffer(1 << 20);
		for (;;)
		{
			const size_t got = stream.Read(buffer.data(), buffer.size());
			if (got == 0) break;
			if (out.size() + got > limit)
			{
				error = "This layer is larger than the repeat proof can hold.";
				return false;
			}
			out.insert(out.end(), buffer.data(), buffer.data() + got);
		}

		if (!stream.Ok())
		{
			error = "The compressed layer ended unexpectedly.";
			return false;
		}
		return true;
	}

	bool DecodeXzBytes(const std::vector<uint8_t>& input, std::vector<uint8_t>& out, size_t limit,
		std::string& error)
	{
		auto source = std::make_unique<BufferStream>(input);
		auto decoder = std::make_unique<XzStream>(std::move(source));
		if (!decoder->Ok())
		{
			error = "The repeated cycle is not an xz stream.";
			return false;
		}

		out.clear();
		return ReadAll(*decoder, out, limit, error);
	}

	void FindXzHeaders(const uint8_t* data, size_t size, std::vector<size_t>& offsets)
	{
		for (size_t i = 0; i + sizeof(SIG_XZ) <= size; i++)
		{
			if (std::memcmp(data + i, SIG_XZ, sizeof(SIG_XZ)) == 0)
			{
				offsets.push_back(i);
				i += sizeof(SIG_XZ) - 1;
			}
		}
	}

	// Reads a layer that is a short prefix followed by byte-identical compressed cycles. Every
	// byte after the prefix is compared with the first cycle, so the proof is over the complete
	// layer, not a sample. Returns false for anything that is not exactly periodic.
	bool ProveCycles(ArchiveStream& layer, std::vector<uint8_t>& preamble, std::vector<uint8_t>& cycle,
		uint64_t& cycles, std::string& error)
	{
		constexpr size_t PROBE_LIMIT = 64ull << 20;

		std::vector<uint8_t> probe;
		probe.reserve(1u << 20);
		std::vector<uint8_t> buffer(1 << 20);
		while (probe.size() < PROBE_LIMIT)
		{
			const size_t got = layer.Read(buffer.data(), buffer.size());
			if (got == 0) break;
			probe.insert(probe.end(), buffer.data(), buffer.data() + got);
		}

		if (!layer.Ok())
		{
			error = "The layer ended unexpectedly while proving repeats.";
			return false;
		}

		std::vector<size_t> headers;
		FindXzHeaders(probe.data(), probe.size(), headers);
		if (headers.size() < 2 || headers[0] != 0)
		{
			error = "No repeated xz cycle was found.";
			return false;
		}

		const size_t start = headers[1];
		if (start < sizeof(SIG_XZ) || start + 64 > probe.size())
		{
			error = "The repeated cycle starts too close to the end of the probe.";
			return false;
		}

		size_t period = 0;
		for (size_t at = start + 64; at + 64 <= probe.size(); at++)
		{
			if (std::memcmp(probe.data() + at, probe.data() + start, 64) == 0)
			{
				period = at - start;
				break;
			}
		}

		// A cycle shorter than 64 bytes cannot be distinguished from the probe's own header.
		if (period < 64 || start + period + 64 > probe.size())
		{
			error = "No complete repeated cycle was found in the probe.";
			return false;
		}

		preamble.assign(probe.begin(), probe.begin() + (std::ptrdiff_t)start);
		cycle.assign(probe.begin() + (std::ptrdiff_t)start,
			probe.begin() + (std::ptrdiff_t)(start + period));

		for (size_t pos = start; pos < probe.size();)
		{
			const size_t chunk = std::min(period, probe.size() - pos);
			if (std::memcmp(probe.data() + pos, cycle.data(), chunk) != 0)
			{
				error = "The repeated region differs from its first cycle.";
				return false;
			}
			pos += chunk;
		}

		uint64_t afterStart = probe.size() - start;
		size_t offset = (size_t)(afterStart % period);
		for (;;)
		{
			const size_t got = layer.Read(buffer.data(), buffer.size());
			if (got == 0) break;

			size_t done = 0;
			while (done < got)
			{
				const size_t chunk = std::min(period - offset, got - done);
				if (std::memcmp(buffer.data() + done, cycle.data() + offset, chunk) != 0)
				{
					error = "A later cycle differs from the first one.";
					return false;
				}
				done += chunk;
				offset += chunk;
				if (offset == period) offset = 0;
			}
			afterStart += got;
		}

		if (!layer.Ok())
		{
			error = "The layer ended unexpectedly while proving repeats.";
			return false;
		}

		if (afterStart == 0 || afterStart % period != 0)
		{
			error = "The repeated region is not a whole number of cycles.";
			return false;
		}

		cycles = afterStart / period;
		if (cycles < 2)
		{
			error = "The layer does not contain at least two cycles.";
			return false;
		}

		return true;
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
		const std::atomic<bool>* cancel, std::vector<uint8_t>& out, std::string& error)
	{
		constexpr size_t RISKY_WINDOW = 2u << 20;

		const uint64_t budget = MemoryBudget();

		// A MIDI entry is never a layer to skip through, and without a declared size there is no
		// "rest of the layer" to fill.
		const bool useRiskyDedupe = g_riskyFastDedupe.load(std::memory_order_relaxed)
			&& declaredSize > 0 && !IsMidiHeader(prefix, prefixSize);
		bool riskyAttempted = false;

		out.clear();
		if (declaredSize > 0 && (uint64_t)declaredSize <= budget)
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

			if (out.size() + (size_t)got > budget)
			{
				// Say why, with numbers: this is the wall a very large nested file hits.
				error = "This file expands past " + HumanBytes(budget) + ", more than Comet can hold in "
					"memory on this machine (the archive is unpacked in memory layer by layer).";
				return false;
			}

			out.insert(out.end(), buffer, buffer + got);

			// Risky fast dedup: a layer built as "header + one tile repeated" does not need to be
			// decoded past the second tile. Nothing is guessed unless the rest of the layer's declared
			// size divides exactly by the tile, which is the case the option is meant for.
			if (useRiskyDedupe && !riskyAttempted && out.size() >= RISKY_WINDOW * 3)
			{
				riskyAttempted = true;

				const size_t tile = SmallestPeriod(out.data() + out.size() - RISKY_WINDOW, RISKY_WINDOW);
				const uint64_t remaining = (uint64_t)declaredSize - (uint64_t)out.size();

				if (tile >= 64 && remaining > 0 && remaining % tile == 0
					&& (uint64_t)declaredSize <= budget)
				{
					const size_t filled = out.size();
					const std::vector<uint8_t> pattern(out.end() - (std::ptrdiff_t)tile, out.end());

					out.resize((size_t)declaredSize);
					for (size_t at = filled; at < out.size(); at += tile)
						std::memcpy(out.data() + at, pattern.data(), std::min(tile, out.size() - at));

					std::cout << "Layered DeDupe (risky): layer of " << declaredSize << " bytes repeats a "
						<< tile << "-byte tile, so the last " << remaining << " bytes were assumed instead of "
						<< "decoded." << std::endl;

					archive_read_data_skip(a);
					return true;
				}
			}
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

			// Refuse a layer that cannot fit before decompressing a single byte of it: a nested file
			// that big would take minutes to produce the same refusal.
			if (declaredSize > 0 && (uint64_t)declaredSize > MemoryBudget())
			{
				error = "This file expands to " + HumanBytes((uint64_t)declaredSize) + ", more than Comet "
					"can hold in memory on this machine (" + HumanBytes(MemoryBudget()) + " available; "
					"archives are unpacked in memory, layer by layer).";
				return false;
			}

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

			std::string readError;
			if (!ReadEntryData(a, header, headerSize, declaredSize, cancel, candidate.data, readError))
			{
				if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
				{
					error = "Loading canceled.";
					return false;
				}

				// A size refusal is final; only a damaged entry is worth skipping past.
				if (!readError.empty())
				{
					error = readError;
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

std::unique_ptr<ArchiveStream> ArchiveLoader::OpenStream(const std::filesystem::path& path, std::string& error)
{
	auto file = std::make_unique<FileStream>(path);
	if (!file->Ok())
	{
		error = "Could not open the file.";
		return nullptr;
	}

	std::unique_ptr<ArchiveStream> current = std::move(file);

	for (int depth = 0; depth < MAX_NESTING; depth++)
	{
		auto layer = std::make_unique<XzStream>(std::move(current));

		uint8_t head[16]{};
		size_t headSize = 0;
		while (headSize < sizeof(head))
		{
			const size_t got = layer->Read(head + headSize, sizeof(head) - headSize);
			if (got == 0) break;
			headSize += got;
		}

		if (!layer->Ok() && headSize == 0)
		{
			error = "This is not an xz stream.";
			return nullptr;
		}

		auto peeked = std::make_unique<PrefixedStream>(std::move(layer), head, headSize);

		if (headSize >= 4 && std::memcmp(head, "MThd", 4) == 0)
			return peeked;

		// Only pure xz chains are streamed; anything else uses the in-memory extractor.
		const bool xz = headSize >= 6 && head[0] == 0xFD && head[1] == '7' && head[2] == 'z'
			&& head[3] == 'X' && head[4] == 'Z' && head[5] == 0x00;
		if (!xz)
		{
			error = "The next layer is not xz.";
			return nullptr;
		}

		current = std::move(peeked);
	}

	error = "The file is nested too deeply.";
	return nullptr;
}

std::unique_ptr<ArchiveStream> ArchiveLoader::OpenMemoryStream(const uint8_t* data, size_t size)
{
	return std::make_unique<BufferStream>(std::vector<uint8_t>(data, data + size));
}

std::unique_ptr<ArchiveStream> ArchiveLoader::OpenProvenRepeatStream(const std::filesystem::path& path,
	RepeatedCycleInfo& info, std::string& error)
{
	info = {};
	error.clear();

	auto firstInput = std::make_unique<FileStream>(path);
	if (!firstInput->Ok())
	{
		error = "Could not open the file.";
		return nullptr;
	}

	auto firstLayer = std::make_unique<XzStream>(std::move(firstInput));
	if (!firstLayer->Ok())
	{
		error = "This is not an xz stream.";
		return nullptr;
	}

	uint8_t head[16]{};
	size_t headSize = 0;
	while (headSize < sizeof(head))
	{
		const size_t got = firstLayer->Read(head + headSize, sizeof(head) - headSize);
		if (got == 0) break;
		headSize += got;
	}

	// A plain song.mid.xz is already the MIDI; the normal loader handles it without probing.
	if (headSize >= 4 && std::memcmp(head, "MThd", 4) == 0)
	{
		return nullptr;
	}

	if (headSize < sizeof(SIG_XZ) || std::memcmp(head, SIG_XZ, sizeof(SIG_XZ)) != 0)
	{
		error = "The first layer is not xz.";
		return nullptr;
	}

	auto restored = std::make_unique<PrefixedStream>(std::move(firstLayer), head, headSize);
	std::vector<uint8_t> layer1;
	if (!ReadAll(*restored, layer1, 1ull << 30, error))
	{
		return nullptr;
	}

	if (IsMidiHeader(layer1.data(), layer1.size()))
	{
		return nullptr;
	}

	if (layer1.size() < sizeof(SIG_XZ) || std::memcmp(layer1.data(), SIG_XZ, sizeof(SIG_XZ)) != 0)
	{
		error = "The second layer is not xz.";
		return nullptr;
	}

	auto secondInput = std::make_unique<BufferStream>(std::move(layer1));
	auto secondLayer = std::make_unique<XzStream>(std::move(secondInput));
	if (!secondLayer->Ok())
	{
		error = "The second layer is not an xz stream.";
		return nullptr;
	}

	std::vector<uint8_t> preamble;
	std::vector<uint8_t> cycle;
	uint64_t cycles = 0;
	if (!ProveCycles(*secondLayer, preamble, cycle, cycles, error))
	{
		return nullptr;
	}

	if (cycles > 65534)
	{
		error = "The repeat count does not fit a MIDI track count.";
		return nullptr;
	}

	std::vector<uint8_t> preambleInner;
	if (!DecodeXzBytes(preamble, preambleInner, 64ull << 20, error))
	{
		return nullptr;
	}

	if (preambleInner.size() < sizeof(SIG_XZ)
		|| std::memcmp(preambleInner.data(), SIG_XZ, sizeof(SIG_XZ)) != 0)
	{
		error = "The preamble is not an xz stream.";
		return nullptr;
	}

	std::vector<uint8_t> midi;
	if (!DecodeXzBytes(preambleInner, midi, 64ull << 20, error))
	{
		return nullptr;
	}

	if (!IsMidiHeader(midi.data(), midi.size()) || midi.size() < 14)
	{
		error = "The preamble does not contain a MIDI header.";
		return nullptr;
	}

	const uint16_t declaredTracks = (uint16_t)(((uint16_t)midi[10] << 8) | (uint16_t)midi[11]);
	if (declaredTracks != cycles + 1)
	{
		error = "The MIDI header does not describe one header track plus one track per cycle.";
		return nullptr;
	}

	std::vector<uint8_t> unit;
	if (!DecodeXzBytes(cycle, unit, 16ull << 20, error))
	{
		return nullptr;
	}

	if (unit.size() < sizeof(SIG_XZ) || std::memcmp(unit.data(), SIG_XZ, sizeof(SIG_XZ)) != 0)
	{
		error = "A repeated cycle does not contain an xz track stream.";
		return nullptr;
	}

	info.cycles = cycles;
	info.declaredTracks = declaredTracks;
	info.preambleMidi = std::move(midi);

	std::cout << "  Layered DeDupe: proved " << cycles << " identical " << cycle.size()
		<< "-byte compressed cycles; only one is decoded." << std::endl;

	auto unitInput = std::make_unique<BufferStream>(std::move(unit));
	auto trackStream = std::make_unique<XzStream>(std::move(unitInput));
	return std::make_unique<PrefixedStream>(std::move(trackStream), info.preambleMidi.data(),
		info.preambleMidi.size());
}

void ArchiveLoader::SetRiskyFastDedupe(bool on)
{
	g_riskyFastDedupe.store(on, std::memory_order_relaxed);
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
