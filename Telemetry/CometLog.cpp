#include "Telemetry/CometLog.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <vector>

namespace CometLog
{
	namespace
	{
		constexpr size_t LEN = MAX_LINE_LENGTH;

		char g_lines[MAX_LINES][LEN];
		uint32_t g_lengths[MAX_LINES] = {};
		// Total lines ever pushed, and the slot the next line goes into.
		std::atomic<uint32_t> g_count{ 0 };
		std::atomic<uint32_t> g_next{ 0 };

		bool g_installed = false;
		std::streambuf* g_coutBuffer = nullptr;
		std::streambuf* g_cerrBuffer = nullptr;

		// Forwards everything written to a stream into the real buffer while also cutting the text
		// into lines for the ring. Allocation-free by design: only the static arrays are touched,
		// and the per-stream pending line lives inline in the object.
		class TeeStreamBuf : public std::streambuf
		{
		public:
			void SetOriginal(std::streambuf* buffer) { original = buffer; }

		protected:
			int_type overflow(int_type c) override
			{
				if (traits_type::eq_int_type(c, traits_type::eof()))
					return traits_type::not_eof(c);

				Append(static_cast<char>(c));
				return original ? original->sputc(static_cast<char>(c)) : c;
			}

			std::streamsize xsputn(const char* s, std::streamsize n) override
			{
				for (std::streamsize i = 0; i < n; ++i)
					Append(s[i]);

				return original ? original->sputn(s, n) : n;
			}

			int sync() override
			{
				FlushPending();
				return original ? original->pubsync() : 0;
			}

		private:
			void Append(char c)
			{
				// Both terminators end a line; the app uses '\r' for in-place progress updates,
				// and those are worth keeping too.
				if (c == '\n' || c == '\r')
				{
					FlushPending();
					return;
				}

				if (pendingLength >= LEN - 1)
					FlushPending();

				pending[pendingLength++] = c;
			}

			void FlushPending()
			{
				if (pendingLength == 0)
					return;

				PushLine(pending, pendingLength);
				pendingLength = 0;
			}

			std::streambuf* original = nullptr;
			char pending[LEN];
			size_t pendingLength = 0;
		};

		TeeStreamBuf g_coutTee;
		TeeStreamBuf g_cerrTee;

		// Slot math shared by PushLine/Snapshot: line number `i` lives in slot i % MAX_LINES.
		size_t SlotOf(uint32_t line) { return static_cast<size_t>(line) % MAX_LINES; }
	}

	void PushLine(const char* text, size_t length)
	{
		if (!text)
			return;

		size_t n = length;
		while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r'))
			--n;

		if (n > LEN - 1)
			n = LEN - 1;

		// Claim the slot first so concurrent writers never land on the same line.
		const uint32_t line = g_next.fetch_add(1, std::memory_order_relaxed);
		const size_t slot = SlotOf(line);

		if (n > 0)
			std::memcpy(g_lines[slot], text, n);

		g_lines[slot][n] = '\0';

		// Publish the length last so a reader never sees a stale slice of a half-written line.
		std::atomic_thread_fence(std::memory_order_release);
		g_lengths[slot] = static_cast<uint32_t>(n);

		g_count.fetch_add(1, std::memory_order_release);
	}

	void PushLine(const std::string& text)
	{
		PushLine(text.data(), text.size());
	}

	size_t Snapshot(char (*outLines)[LEN], size_t maxLines)
	{
		if (!outLines || maxLines == 0)
			return 0;

		const uint32_t count = g_count.load(std::memory_order_acquire);
		if (count == 0)
			return 0;

		const size_t available = count < MAX_LINES ? count : MAX_LINES;
		const size_t take = available < maxLines ? available : maxLines;
		// The newest `take` lines are numbered [count - take, count).
		const uint32_t begin = count - static_cast<uint32_t>(take);

		for (size_t i = 0; i < take; ++i)
		{
			const size_t slot = SlotOf(begin + static_cast<uint32_t>(i));
			uint32_t len = g_lengths[slot];
			if (len > LEN - 1)
				len = static_cast<uint32_t>(LEN - 1);

			std::memcpy(outLines[i], g_lines[slot], len);
			outLines[i][len] = '\0';
		}

		return take;
	}

	std::string Tail(size_t maxLines)
	{
		if (maxLines == 0)
			return {};

		if (maxLines > MAX_LINES)
			maxLines = MAX_LINES;

		std::vector<char> storage(maxLines * LEN);
		auto* lines = reinterpret_cast<char(*)[LEN]>(storage.data());
		const size_t count = Snapshot(lines, maxLines);

		std::string out;
		out.reserve(count * 32);

		for (size_t i = 0; i < count; ++i)
		{
			out += lines[i];
			out += '\n';
		}

		return out;
	}

	void Install()
	{
		if (g_installed)
			return;

		g_installed = true;

		g_coutBuffer = std::cout.rdbuf();
		g_cerrBuffer = std::cerr.rdbuf();

		g_coutTee.SetOriginal(g_coutBuffer);
		g_cerrTee.SetOriginal(g_cerrBuffer);

		std::cout.rdbuf(&g_coutTee);
		std::cerr.rdbuf(&g_cerrTee);
	}

	void Uninstall()
	{
		if (!g_installed)
			return;

		g_installed = false;

		std::cout.rdbuf(g_coutBuffer);
		std::cerr.rdbuf(g_cerrBuffer);
	}
}
