#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// The pieces the rendered-ahead engine needs to play audio kestrel is still writing:
//   * kestrel 1.3.0+ writes the audio to "<name>.partial.<ext>" and renames it when it is whole,
//     so the data chunk grows while the render runs and its header may still say 0;
//   * "kestrel --progress json" prints one JSON object per line carrying "audio_secs".
// Keeping both here makes them testable without a GPU, a sound device or a MIDI file.
namespace RenderAhead
{
	struct WavInfo
	{
		uint32_t dataOffset = 0;   // where the sample bytes start
		uint32_t channels = 0;
		uint32_t rate = 0;
		uint32_t bits = 0;
		bool valid = false;
	};

	inline uint16_t ReadU16(const uint8_t* data)
	{
		return (uint16_t)(data[0] | (data[1] << 8));
	}

	inline uint32_t ReadU32(const uint8_t* data)
	{
		return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
			((uint32_t)data[3] << 24);
	}

	// Walks the RIFF chunks rather than assuming a 44-byte header: kestrel's writer may put other
	// chunks before "data", and only the positions matter here. The data chunk's declared size is
	// deliberately ignored, because it is 0 until the render ends.
	inline bool ParseWavHeader(const uint8_t* data, size_t size, WavInfo& info)
	{
		info = WavInfo{};

		if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
			return false;

		uint32_t offset = 12;
		bool haveFormat = false;

		while (offset + 8 <= size)
		{
			const uint32_t chunkSize = ReadU32(data + offset + 4);
			const uint8_t* chunk = data + offset + 8;

			if (std::memcmp(data + offset, "fmt ", 4) == 0)
			{
				if (chunkSize < 16 || offset + 8 + 16 > size) return false;

				info.channels = ReadU16(chunk + 2);
				info.rate = ReadU32(chunk + 4);
				info.bits = ReadU16(chunk + 14);
				haveFormat = true;
			}
			else if (std::memcmp(data + offset, "data", 4) == 0)
			{
				if (!haveFormat || info.channels == 0 || info.rate == 0 || info.bits == 0)
					return false;

				info.dataOffset = offset + 8;
				info.valid = true;
				return true;
			}

			offset += 8 + chunkSize + (chunkSize & 1u);
		}

		return false;
	}

	inline uint64_t BytesPerFrame(const WavInfo& info)
	{
		return (uint64_t)info.channels * ((info.bits + 7) / 8);
	}

	// How much audio actually exists on disk. The header's sizes describe the finished file, so the
	// file's real length is what decides whether a frame can be played.
	inline uint64_t AvailableFrames(uint64_t fileSize, const WavInfo& info)
	{
		if (!info.valid) return 0;

		const uint64_t frameBytes = BytesPerFrame(info);
		if (frameBytes == 0 || fileSize <= info.dataOffset) return 0;

		return (fileSize - info.dataOffset) / frameBytes;
	}

	// {"type": "progress", ..., "audio_secs": 12.5, ...} - a null before anything is rendered.
	inline bool ParseAudioSeconds(const std::string& line, double& seconds)
	{
		const std::string key = "\"audio_secs\"";
		const size_t at = line.find(key);
		if (at == std::string::npos) return false;

		size_t pos = line.find(':', at + key.size());
		if (pos == std::string::npos) return false;

		pos++;
		while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;

		if (pos >= line.size() || line[pos] == 'n' || line[pos] == 'N') return false; // null

		char* end = nullptr;
		const double value = std::strtod(line.c_str() + pos, &end);
		if (end == line.c_str() + pos) return false;

		seconds = value;
		return true;
	}

	// The line that says the render is over, whatever the key spacing in the JSON.
	inline bool IsRenderEnd(const std::string& line)
	{
		return line.find("\"summary\"") != std::string::npos ||
			line.find("\"failed\"") != std::string::npos;
	}
}
