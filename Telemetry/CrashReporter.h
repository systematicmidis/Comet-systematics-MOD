#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Crash reporting.
//
// When Comet dies, the handler captures what went wrong and where, writes a self-contained JSON
// report, then hands it to a short-lived copy of Comet (relaunched with "--send-crash-reports") so
// the upload never runs inside the dying process. Anything that copy can't deliver stays queued on
// disk and is retried on the next launch.
//
// App state arrives through the setters below and is stored as plain data behind a mutex. Nothing
// is ever *pulled* from the app while handling a crash, because by then those objects may already
// be gone.
namespace CrashReporter
{
	// Where a report came from. "crash" is the real thing; "test" is the settings button; "manual"
	// is reserved for a user-triggered report.
	constexpr const char* KIND_CRASH = "crash";
	constexpr const char* KIND_TEST = "test";
	constexpr const char* KIND_MANUAL = "manual";

	// The endpoint is supplied at build time (COMET_CRASH_WEBHOOK) or at runtime through the
	// COMET_CRASH_WEBHOOK environment variable. It is intentionally not stored in the source tree:
	// a public webhook URL is a credential, and anyone who has it can post to the channel.
	constexpr const char* WEBHOOK_URL_ENV = "COMET_CRASH_WEBHOOK";
	std::string DefaultWebhookUrl();

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

	struct FlushResult
	{
		int sent = 0;
		int failed = 0;
		int remaining = 0;
		std::string lastError;
	};

	// Installs the SEH / terminate / abort handlers and prepares symbol lookup. Call as early as
	// possible in main(). Reporting stays dormant until SetEnabled(true).
	void Install();

	void SetAppInfo(std::string version, std::string buildType, std::string buildConfig);
	void SetGraphicsInfo(std::string vendor, std::string renderer, std::string version, std::string glVersion);

	// Snapshots pushed by the app whenever they change.
	void SetMidiInfo(const MidiInfo& info);
	void ClearMidiInfo();
	void SetRuntimeFlags(const RuntimeFlags& flags);
	void SetSettingsSnapshot(std::string json);

	// Master switch: while false, crashes are not captured at all.
	void SetEnabled(bool enabled);

	// Overrides the endpoint in this process. Test hook only; empty disables uploads.
	void SetWebhookUrl(std::string url);

	// Folder holding pending/, sent/ and failed/. Overridable with COMET_CRASH_REPORT_DIR.
	std::string ReportDirectory();

	// The full report document, including the "_delivery" block the sender reads and strips.
	std::string BuildReport(const std::string& kind, const std::string& note = {});

	// The Discord webhook body for a report (embed + attachment reference). Exposed so the
	// settings dialog can preview exactly what would be uploaded.
	std::string BuildDiscordPayload(const std::string& reportJson);

	// Uploads `reportJson` to a Discord webhook. Returns false and fills `errorOut` on failure.
	bool PostToDiscord(const std::string& webhookUrl, const std::string& reportJson, std::string* errorOut);

	// Delivers a report, queueing it for retry if the upload fails.
	bool SendOrQueue(const std::string& reportJson, const std::string& webhookUrl, std::string* errorOut);

	// Retries everything queued. `webhookUrl` overrides the report's own endpoint when non-empty;
	// otherwise the report's endpoint is used, and finally config.yml.
	FlushResult FlushPending(const std::string& webhookUrl = {});

	// Number of reports waiting to be sent.
	int CountPending();
	// Deletes every queued report. Returns how many were removed.
	int DeletePending();

	// Starts a background retry of the queue using config.yml. Called once at startup.
	void FlushPendingInBackground();

	// Entry point for the helper process ("--send-crash-reports").
	int RunSenderMode();

	// The endpoint reports go to, used when a queued report carries none of its own. Empty means
	// the build/environment has no endpoint configured and reports are queued locally only.
	std::string WebhookUrl();

	// Deliberately dereferences a null pointer so the whole pipeline can be exercised in tests.
	void CrashForTesting();
}
