#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "UpdateChecker.h"

// Help > Check for Updates > "Build and install": fetches the selected branch's source, compiles it
// with the Visual Studio C++ tools already installed on this machine, keeps only the resulting
// Comet.exe and swaps it in place of the running one. The source tree, the CMake build directory
// and the downloaded zip are deleted afterwards, so a finished build leaves nothing behind but the
// exe (and a log, kept in case a future build needs troubleshooting).
class UpdateBuilder : public std::enable_shared_from_this<UpdateBuilder>
{
public:
	enum class Stage
	{
		Idle,
		Resolving,
		Downloading,
		Extracting,
		Configuring,
		Building,
		Installing,
		Done,
		Failed,
		Cancelled
	};

	struct Status
	{
		Stage stage = Stage::Idle;
		float progress = -1.0f; // 0..1 while the stage can report it, -1 when it cannot
		double elapsed = 0.0;   // seconds spent in the current stage
		std::string message;    // what is happening right now
		std::string error;      // set when stage == Failed
		std::string logPath;    // compiler output, kept after a failure
		bool exeReady = false;  // the new exe is in place and takes effect on the next start
	};

	// Release is the normal build; Debug keeps assertions and symbols. Whichever is produced is
	// installed under the name that says which it is, so Comet-Debug.exe is always a debug build.
	enum class Configuration { Release, Debug };

	struct Options
	{
		UpdateChecker::Source source = UpdateChecker::Source::Official;
		Configuration configuration = Configuration::Release;
		bool downloadExe = false; // take the release's prebuilt exe instead of compiling
	};

	static std::shared_ptr<UpdateBuilder> Create();

	// Starts the pipeline on a worker thread. Does nothing while a build is already running.
	void Start(const Options& options);

	static std::string ExeNameFor(Configuration configuration);
	static bool IsDebugBuild(const std::filesystem::path& exe);
	void Cancel();
	bool IsRunning() const { return running; }
	Status GetStatus() const;

	static const char* StageName(Stage stage);

	// The Comet.exe this process was started from, and the folder holding it.
	static std::filesystem::path GetRunningExePath();
	// %TEMP%\CometUpdate, the scratch folder a build works in.
	static std::filesystem::path GetWorkRoot();

	// Deletes the previous exe an installed update left behind. Only possible now that it is no
	// longer running, so this is called once from main().
	static void RemoveStaleInstall();

private:
	UpdateBuilder() = default;

	void Run(const Options& options);

	bool DownloadReleaseExe();

	bool PrepareToolchain();
	bool Configure();
	bool Compile();
	bool Download(const std::string& url, const std::filesystem::path& dest);
	bool Extract(const std::filesystem::path& zip, const std::filesystem::path& dest);
	bool InstallExe(const std::filesystem::path& newExe);
	void CleanWorkFiles();

	void SetStage(Stage newStage, const std::string& text, float progress = -1.0f);
	void SetMessage(const std::string& text);
	void Fail(const std::string& text);

	std::atomic<Stage> stage{ Stage::Idle };
	std::atomic<float> progress{ -1.0f };
	std::atomic<bool> cancelRequested{ false };
	std::atomic<bool> running{ false };
	std::atomic<bool> exeReady{ false };
	std::atomic<double> stageStart{ 0.0 };

	mutable std::mutex mutex;
	std::string message;
	std::string error;
	std::string logPath;

	std::filesystem::path workRoot;
	std::filesystem::path sourceDir;
	std::filesystem::path buildDir;
	std::filesystem::path builtExe;
	std::filesystem::path downloadedExe;
	std::string installedName;
	bool installedInPlace = false;

	UpdateChecker::Source source = UpdateChecker::Source::Official;
	Configuration configuration = Configuration::Release;
	bool downloadExe = false;

	// Resolved once by PrepareToolchain so Configure and Compile agree on them.
	std::filesystem::path cmakePath;
	std::filesystem::path vcvarsPath;
	std::filesystem::path gitPath;
	std::filesystem::path ninjaPath;
	std::string generator;
	bool useNinja = false;
};
