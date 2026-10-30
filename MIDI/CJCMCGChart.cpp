#include "CJCMCGChart.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
	// All multi-byte values in a CJCMCG file are big-endian.
	struct BigEndianReader
	{
		const uint8_t* p;
		const uint8_t* end;
		bool ok = true;

		size_t Remaining() const { return (size_t)(end - p); }
		bool Has(size_t n) const { return p + n <= end; }

		uint32_t U32()
		{
			if (!Has(4)) { ok = false; p = end; return 0; }
			const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
			p += 4;
			return v;
		}

		// BPM data is microseconds per quarter note, stored in three bytes.
		uint32_t U24()
		{
			if (!Has(3)) { ok = false; p = end; return 0; }
			const uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
			p += 3;
			return v;
		}

		// 64-bit so a declared note count in the trillions survives.
		uint64_t VLQ()
		{
			uint64_t value = 0;
			for (int i = 0; i < 10; i++)
			{
				if (!Has(1)) { ok = false; return value; }
				const uint8_t b = *p++;
				value = (value << 7) | (uint64_t)(b & 0x7F);
				if ((b & 0x80) == 0) return value;
			}
			ok = false;
			return value;
		}

		void Skip(size_t n)
		{
			if (!Has(n)) { ok = false; p = end; return; }
			p += n;
		}
	};

	int ToLower(int c) { return std::tolower((unsigned char)c); }

	constexpr double DEFAULT_USEC_PER_QUARTER = 500000.0; // 120 BPM when the file has no tempo
}

bool CJCMCGChart::IsCJCMCGFile(const std::string& path)
{
	std::string ext = std::filesystem::path(path).extension().string();
	if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
	std::transform(ext.begin(), ext.end(), ext.begin(), ToLower);
	return ext == EXTENSION;
}

uint64_t CJCMCGChart::NotesStartedAt(double tick) const
{
	if (tick <= 0.0 || cumOn.empty()) return 0;
	const size_t idx = std::min<size_t>((size_t)tick, cumOn.size() - 1);
	return cumOn[idx];
}

uint64_t CJCMCGChart::NotesEndedAt(double tick) const
{
	if (tick <= 0.0 || cumOff.empty()) return 0;
	const size_t idx = std::min<size_t>((size_t)tick, cumOff.size() - 1);
	return cumOff[idx];
}

uint64_t CJCMCGChart::PolyphonyAt(double tick) const
{
	const uint64_t on = NotesStartedAt(tick);
	const uint64_t off = NotesEndedAt(tick);
	return on > off ? on - off : 0;
}

double CJCMCGChart::TickToSeconds(double tick) const
{
	if (tick <= 0.0) return 0.0;
	if (ppq == 0) return 0.0;
	if (tempos.empty()) return tick * 60.0 / (120.0 * (double)ppq);

	double secs = 0.0;
	double prevTick = 0.0;
	double usec = DEFAULT_USEC_PER_QUARTER;

	for (const TempoEvent& tempo : tempos)
	{
		const double t = (double)tempo.tick;
		const double seg = std::min(tick, t) - prevTick;
		if (seg > 0.0) secs += seg * usec / ((double)ppq * 1.0e6);

		if (tick <= t) return secs;

		if (t > prevTick) prevTick = t;
		usec = 6.0e7 / std::max(tempo.bpm, 1.0e-6);
	}

	secs += (tick - prevTick) * usec / ((double)ppq * 1.0e6);
	return secs;
}

double CJCMCGChart::SecondsToTick(double seconds) const
{
	if (seconds <= 0.0) return 0.0;
	if (ppq == 0) return 0.0;
	if (tempos.empty()) return seconds * 120.0 * (double)ppq / 60.0;

	double secs = 0.0;
	double prevTick = 0.0;
	double usec = DEFAULT_USEC_PER_QUARTER;

	for (const TempoEvent& tempo : tempos)
	{
		const double t = (double)tempo.tick;
		const double segTicks = t - prevTick;
		if (segTicks > 0.0)
		{
			const double segSecs = segTicks * usec / ((double)ppq * 1.0e6);
			if (seconds <= secs + segSecs)
				return prevTick + (seconds - secs) * ((double)ppq * 1.0e6) / usec;

			secs += segSecs;
		}

		if (t > prevTick) prevTick = t;
		usec = 6.0e7 / std::max(tempo.bpm, 1.0e-6);
	}

	return prevTick + (seconds - secs) * ((double)ppq * 1.0e6) / usec;
}

std::shared_ptr<CJCMCGChart> CJCMCGChart::Parse(const uint8_t* data, size_t size)
{
	if (data == nullptr || size == 0)
		throw std::runtime_error("CJCMCG: empty file");

	BigEndianReader r{ data, data + size };
	auto chart = std::make_shared<CJCMCGChart>();

	uint32_t ppq = r.U32();
	if (!r.ok) throw std::runtime_error("CJCMCG: truncated header");
	if (ppq == 0) ppq = 384;
	chart->ppq = ppq;

	const uint32_t bpmCount = r.U32();
	if (!r.ok) throw std::runtime_error("CJCMCG: truncated BPM count");
	for (uint32_t i = 0; i < bpmCount; i++)
	{
		const uint32_t tick = r.U32();
		const uint32_t usecs = r.U24();
		if (!r.ok) throw std::runtime_error("CJCMCG: truncated BPM chunk");
		if (usecs == 0) continue;
		chart->tempos.emplace_back((long)tick, 6.0e7 / (double)usecs);
	}
	std::sort(chart->tempos.begin(), chart->tempos.end(),
		[](const TempoEvent& a, const TempoEvent& b) { return a.tick < b.tick; });

	// Lyrics are skipped; nothing downstream consumes them.
	const uint32_t lyricCount = r.U32();
	if (!r.ok) throw std::runtime_error("CJCMCG: truncated lyric count");
	chart->lyricCount = lyricCount;
	for (uint32_t i = 0; i < lyricCount; i++)
	{
		r.U32(); // event tick
		const uint32_t len = r.U32();
		if (!r.ok) throw std::runtime_error("CJCMCG: truncated lyric chunk");
		if (r.Remaining() < (size_t)len)
			throw std::runtime_error("CJCMCG: truncated lyric chunk");
		r.Skip(len);
	}

	const uint64_t declaredNotes = r.VLQ();
	const uint32_t tickCount = r.U32();
	if (!r.ok) throw std::runtime_error("CJCMCG: truncated note counts");

	chart->tickCount = tickCount;
	chart->totalNotes = declaredNotes;
	chart->listedEntries = tickCount;
	chart->cumOn.assign((size_t)tickCount + 1, 0);
	chart->cumOff.assign((size_t)tickCount + 1, 0);

	uint64_t on = 0;
	uint64_t off = 0;
	for (uint32_t i = 0; i < tickCount; i++)
	{
		const uint64_t notesOn = r.VLQ();
		const uint64_t notesOff = r.VLQ();
		if (!r.ok) throw std::runtime_error("CJCMCG: truncated note data");

		on += notesOn;
		off += notesOff;
		chart->cumOn[(size_t)i + 1] = on;
		chart->cumOff[(size_t)i + 1] = off;
	}

	if (declaredNotes == 0)
		chart->totalNotes = on;

	std::cout << "CJCMCG chart: " << tickCount << " ticks, " << on << " notes over "
		<< chart->LengthSeconds() << "s" << std::endl;

	return chart;
}

std::shared_ptr<CJCMCGChart> CJCMCGChart::LoadFile(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		throw std::runtime_error("CJCMCG: could not open " + path);

	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return Parse(bytes.data(), bytes.size());
}
