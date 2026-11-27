#pragma once

#include <cstdint>
#include <string>

// Crash logging.
//
// When Comet dies, the handler captures what went wrong and where and writes a plain-text .log
// file into the app's logs folder. Nothing is uploaded: the user can open the folder from
// Settings and send the file in the Systematics Discord when reporting the crash.
//
// App state arrives through the setters below and is stored as plain data behind a mutex.
// Nothing is ever *pulled* from the app while handling a crash, because by then those objects
// may already be gone.
namespace CrashReporter
{
	// Where a log came from. "crash" is the real thing; "test" is the settings button.
	constexpr const char* KIND_CRASH = "crash";
	constexpr const char* KIND_TEST = "test";

	struct MidiInfo
	{
		std::string path;
		std::string displayName;
		long long notes = 0;
		long long lengthTicks = 0;
		uint16_t trackCount = 0;
		uint16_t noteTrackCount = 0;
		uint16_t resolution = 0;
		double lengthSeconds = 0.0;
	};

	struct RuntimeFlags
	{
		bool hasSequence = false;
		bool playing = false;
		bool rendering = false;
		bool loading = false;
	};

	// Installs the SEH / terminate / abort handlers and prepares symbol lookup. Call as early as
	// possible in main(). Logging stays dormant until SetEnabled(true).
	void Install();

	void SetAppInfo(std::string version, std::string buildType, std::string buildConfig);
	void SetGraphicsInfo(std::string vendor, std::string renderer, std::string version, std::string glVersion);

	// Snapshots pushed by the app whenever they change.
	void SetMidiInfo(const MidiInfo& info);
	void ClearMidiInfo();
	void SetRuntimeFlags(const RuntimeFlags& flags);
	void SetSettingsSnapshot(std::string json);

	// Master switch: while false, crashes do not write a log.
	void SetEnabled(bool enabled);

	// Folder holding the .log files. Created on demand.
	std::string LogDirectory();

	// Writes one report to LogDirectory and returns its full path, or an empty string when the
	// file could not be written. Used by the settings dialog's test button.
	std::string WriteLog(const std::string& kind, const std::string& note = {});

	// Deliberately dereferences a null pointer so the whole pipeline can be exercised in tests.
	void CrashForTesting();
}
