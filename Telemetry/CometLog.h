#pragma once

#include <cstddef>
#include <string>

// Keeps the tail of everything Comet prints so a crash report can carry the run-up to the failure.
//
// The backing store is a fixed-size static array written through atomics only, so an exception
// handler can read it without allocating or locking - both of which are unsafe once the process is
// already dying. Lines that overflow a slot are truncated rather than grown.
namespace CometLog
{
	constexpr size_t MAX_LINES = 256;
	constexpr size_t MAX_LINE_LENGTH = 320;

	// Tees std::cout and std::cerr into the ring buffer. Call once, as early as possible.
	void Install();
	// Restores the original stream buffers. Optional; the process exiting does the same thing.
	void Uninstall();

	// Appends one line. Trailing newlines are stripped and over-long lines are truncated.
	void PushLine(const char* text, size_t length);
	void PushLine(const std::string& text);

	// Copies the most recent `maxLines` lines into outLines, oldest first, and returns how many
	// were written. Each entry is NUL-terminated. Safe to call from an exception handler.
	size_t Snapshot(char (*outLines)[MAX_LINE_LENGTH], size_t maxLines);

	// The same tail as one newline-joined string, for report bodies and the settings preview.
	std::string Tail(size_t maxLines = MAX_LINES);
}
