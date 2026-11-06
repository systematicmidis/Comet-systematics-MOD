#include "UpdateBuilder.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>

namespace
{
	constexpr const wchar_t* EXE_NAME = L"Comet.exe";

	double NowSeconds()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	std::filesystem::path FindTool(const wchar_t* name)
	{
		std::array<wchar_t, MAX_PATH * 2> buffer{};
		const DWORD length = SearchPathW(nullptr, name, L".exe", DWORD(buffer.size()), buffer.data(), nullptr);
		if (length == 0 || length >= buffer.size())
			return {};

		return std::filesystem::path(buffer.data());
	}

	std::string ReadLogTail(const std::filesystem::path& path, std::size_t maxChars)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return {};

		in.seekg(0, std::ios::end);
		const std::streamoff size = in.tellg();
		const std::streamoff start = size > std::streamoff(maxChars) ? size - std::streamoff(maxChars) : 0;
		in.seekg(start, std::ios::beg);

		std::string text(std::size_t(size - start), '\0');
		in.read(text.data(), std::streamsize(text.size()));
		return text;
	}

	// Runs a command with stdout and stderr redirected into `log`, hidden. onPoll runs between
	// wait ticks so a long compile can update the progress bar. Returns -1 when the process could
	// not start, -2 when it was killed through cancel, otherwise its exit code.
	int RunHidden(const std::wstring& commandLine, const std::filesystem::path& log,
		const std::atomic<bool>& cancel, const std::function<void()>& onPoll = {})
	{
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = TRUE;

		HANDLE file = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return -1;

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = file;
		startup.hStdError = file;
		startup.hStdInput = nullptr;

		PROCESS_INFORMATION process{};
		std::wstring mutableCommand = commandLine;
		const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
		CloseHandle(file);

		if (!started)
			return -1;

		bool cancelled = false;
		while (WaitForSingleObject(process.hProcess, 250) == WAIT_TIMEOUT)
		{
			if (cancel.load())
			{
				cancelled = true;
				// cmake spawned cl.exe and link.exe, so the whole tree has to go.
				std::wstring kill = L"cmd.exe /c taskkill /F /T /PID " + std::to_wstring(process.dwProcessId);
				STARTUPINFOW killStartup{};
				killStartup.cb = sizeof(killStartup);
				PROCESS_INFORMATION killProcess{};
				if (CreateProcessW(nullptr, kill.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
					nullptr, &killStartup, &killProcess))
				{
					WaitForSingleObject(killProcess.hProcess, 5000);
					CloseHandle(killProcess.hProcess);
					CloseHandle(killProcess.hThread);
				}

				WaitForSingleObject(process.hProcess, 5000);
				break;
			}

			if (onPoll)
				onPoll();
		}

		DWORD exitCode = 1;
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);

		return cancelled ? -2 : int(exitCode);
	}

	struct DownloadState
	{
		const std::atomic<bool>* cancel = nullptr;
		std::atomic<float>* progress = nullptr;
	};

	int DownloadProgress(void* clientp, curl_off_t total, curl_off_t done, curl_off_t, curl_off_t)
	{
		auto* state = static_cast<DownloadState*>(clientp);
		if (state->cancel != nullptr && state->cancel->load())
			return 1; // makes curl abort with CURLE_ABORTED_BY_CALLBACK

		if (total > 0 && state->progress != nullptr)
			state->progress->store(float(double(done) / double(total)));

		return 0;
	}

	size_t DownloadWrite(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		auto* file = static_cast<FILE*>(userdata);
		return std::fwrite(ptr, size, nmemb, file);
	}

	// GitHub's zip keeps everything under one folder named after the branch; that folder is the
	// CMake source directory.
	std::filesystem::path SingleRoot(const std::filesystem::path& directory)
	{
		std::error_code ec;
		std::filesystem::path only;
		for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
		{
			if (!only.empty())
				return directory;

			only = entry.path();
		}

		if (!only.empty() && std::filesystem::is_directory(only))
			return only;

		return directory;
	}

	std::string FirstNonEmptyLine(const std::string& text)
	{
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (!line.empty())
				return line;
		}

		return {};
	}

	std::wstring Widen(const std::string& text)
	{
		if (text.empty())
			return {};

		const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
		std::wstring result(std::size_t(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length);
		return result;
	}
}

std::shared_ptr<UpdateBuilder> UpdateBuilder::Create()
{
	return std::shared_ptr<UpdateBuilder>(new UpdateBuilder());
}

const char* UpdateBuilder::StageName(Stage stage)
{
	switch (stage)
	{
	case Stage::Idle: return "Idle";
	case Stage::Resolving: return "Looking up the newest commit";
	case Stage::Downloading: return "Downloading the source";
	case Stage::Extracting: return "Unpacking the source";
	case Stage::Configuring: return "Configuring the build";
	case Stage::Building: return "Compiling Comet";
	case Stage::Installing: return "Installing the new exe";
	case Stage::Done: return "Finished";
	case Stage::Failed: return "Build failed";
	case Stage::Cancelled: return "Cancelled";
	}

	return "Unknown";
}

UpdateBuilder::Status UpdateBuilder::GetStatus() const
{
	Status status;
	status.stage = stage.load();
	status.progress = progress.load();
	status.exeReady = exeReady.load();

	{
		std::lock_guard lock(mutex);
		status.message = message;
		status.error = error;
		status.logPath = logPath;
	}

	const double started = stageStart.load();
	status.elapsed = started > 0.0 ? NowSeconds() - started : 0.0;
	return status;
}

std::filesystem::path UpdateBuilder::GetRunningExePath()
{
	std::array<wchar_t, MAX_PATH * 4> buffer{};
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
	if (length == 0 || length >= buffer.size())
		return {};

	return std::filesystem::path(buffer.data());
}

std::filesystem::path UpdateBuilder::GetWorkRoot()
{
	std::array<wchar_t, MAX_PATH> buffer{};
	const DWORD length = GetEnvironmentVariableW(L"TEMP", buffer.data(), DWORD(buffer.size()));
	if (length == 0 || length >= buffer.size())
		return std::filesystem::temp_directory_path() / "CometUpdate";

	return std::filesystem::path(buffer.data()) / "CometUpdate";
}

void UpdateBuilder::RemoveStaleInstall()
{
	const std::filesystem::path exe = GetRunningExePath();
	if (exe.empty())
		return;

	std::error_code ec;
	// The previous exe of an installed update: the file is still mapped by the process that was
	// running when it got swapped out, which is this one, so it can only be removed on a later start
	// - which is exactly when this runs.
	std::filesystem::remove(std::filesystem::path(exe.wstring() + L".old"), ec);
	// A leftover staged exe means the swap never completed; the running one is intact.
	std::filesystem::remove(std::filesystem::path(exe.wstring() + L".new"), ec);
}

void UpdateBuilder::Start(UpdateChecker::Source source)
{
	if (running.exchange(true))
		return;

	cancelRequested = false;
	exeReady = false;
	progress = -1.0f;
	stage = Stage::Idle;
	{
		std::lock_guard lock(mutex);
		error.clear();
		message.clear();
	}

	// The worker owns a reference, so the build keeps going even if the dialog is closed.
	std::shared_ptr<UpdateBuilder> self = shared_from_this();
	std::thread([self, source]() { self->Run(source); }).detach();
}

void UpdateBuilder::Cancel()
{
	cancelRequested = true;
}

void UpdateBuilder::SetMessage(const std::string& text)
{
	std::lock_guard lock(mutex);
	message = text;
}

void UpdateBuilder::SetStage(Stage newStage, const std::string& text, float prog)
{
	stage = newStage;
	stageStart = NowSeconds();
	progress = prog;
	SetMessage(text);
}

void UpdateBuilder::Fail(const std::string& text)
{
	stage = Stage::Failed;
	progress = -1.0f;

	std::lock_guard lock(mutex);
	error = text;
	message = "The build did not finish.";
}

void UpdateBuilder::Run(UpdateChecker::Source source)
{
	workRoot = GetWorkRoot();
	sourceDir = workRoot / "src";
	buildDir = workRoot / "build";

	{
		std::lock_guard lock(mutex);
		logPath = (workRoot / "build.log").string();
	}

	std::error_code ec;
	std::filesystem::remove_all(workRoot, ec);
	std::filesystem::create_directories(workRoot, ec);

	auto stopCancelled = [this]()
	{
		SetStage(Stage::Cancelled, "Build cancelled.");
		running = false;
	};

	auto stopFailed = [this]() { running = false; };

	SetStage(Stage::Resolving, "Looking up the newest commit...");
	const UpdateChecker::SourcePackage package = UpdateChecker::ResolvePackage(source);
	if (!package.ok)
	{
		Fail(package.error);
		stopFailed();
		return;
	}

	if (cancelRequested)
	{
		stopCancelled();
		return;
	}

	const std::string shortSha = package.sha.substr(0, std::min<std::size_t>(package.sha.size(), 7));
	const std::filesystem::path zip = workRoot / "source.zip";

	SetStage(Stage::Downloading, "Downloading " + shortSha + "...", 0.0f);
	if (!Download(package.zipUrl, zip))
	{
		if (cancelRequested) stopCancelled(); else stopFailed();
		return;
	}

	SetStage(Stage::Extracting, "Unpacking the source...", 0.0f);
	if (!Extract(zip, sourceDir))
	{
		if (cancelRequested) stopCancelled(); else stopFailed();
		return;
	}

	if (!PrepareToolchain())
	{
		stopFailed();
		return;
	}

	SetStage(Stage::Configuring, "Configuring the build (the dependencies are downloaded here)...", 0.0f);
	if (!Configure())
	{
		if (cancelRequested) stopCancelled(); else stopFailed();
		return;
	}

	SetStage(Stage::Building, "Compiling Comet...", 0.0f);
	if (!Compile())
	{
		if (cancelRequested) stopCancelled(); else stopFailed();
		return;
	}

	SetStage(Stage::Installing, "Installing the new Comet.exe...", 0.0f);
	if (!Install())
	{
		stopFailed();
		return;
	}

	CleanWorkFiles();

	exeReady = true;
	SetStage(Stage::Done, "Installed " + shortSha + ". Restart Comet to use it.");
	running = false;
}

bool UpdateBuilder::Download(const std::string& url, const std::filesystem::path& dest)
{
	FILE* file = _wfopen(dest.wstring().c_str(), L"wb");
	if (file == nullptr)
	{
		Fail("Could not create the download file in " + workRoot.string() + ".");
		return false;
	}

	CURL* curl = curl_easy_init();
	if (curl == nullptr)
	{
		std::fclose(file);
		Fail("Could not initialise the downloader.");
		return false;
	}

	DownloadState state;
	state.cancel = &cancelRequested;
	state.progress = &progress;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Comet");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DownloadWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);

	const CURLcode code = curl_easy_perform(curl);
	long httpStatus = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
	curl_easy_cleanup(curl);
	std::fclose(file);

	if (cancelRequested || code == CURLE_ABORTED_BY_CALLBACK)
	{
		std::error_code ec;
		std::filesystem::remove(dest, ec);
		return false;
	}

	if (code != CURLE_OK || httpStatus != 200)
	{
		Fail("Downloading the source failed (" + std::string(curl_easy_strerror(code)) + ", HTTP " +
			std::to_string(httpStatus) + ").");
		return false;
	}

	progress = 1.0f;
	return true;
}

bool UpdateBuilder::Extract(const std::filesystem::path& zip, const std::filesystem::path& dest)
{
	struct archive* reader = archive_read_new();
	archive_read_support_filter_all(reader);
	archive_read_support_format_all(reader);

	if (archive_read_open_filename_w(reader, zip.wstring().c_str(), 1 << 16) != ARCHIVE_OK)
	{
		const char* reason = archive_error_string(reader);
		Fail(std::string("The downloaded archive could not be opened") + (reason ? std::string(": ") + reason : "."));
		archive_read_free(reader);
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(dest, ec);

	struct archive_entry* entry = nullptr;
	std::array<char, 1 << 16> buffer{};
	float files = 0.0f;

	while (archive_read_next_header(reader, &entry) == ARCHIVE_OK)
	{
		if (cancelRequested)
		{
			archive_read_free(reader);
			return false;
		}

		const char* rawName = archive_entry_pathname(entry);
		if (rawName == nullptr)
			continue;

		std::string name = rawName;
		std::replace(name.begin(), name.end(), '\\', '/');

		// Zip entries may try to escape the folder; only relative paths are accepted.
		if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos ||
			name.find("..") != std::string::npos)
			continue;

		const std::filesystem::path target = dest / std::filesystem::u8path(name);
		if (archive_entry_filetype(entry) == AE_IFDIR)
		{
			std::filesystem::create_directories(target, ec);
			continue;
		}

		std::filesystem::create_directories(target.parent_path(), ec);
		std::ofstream out(target, std::ios::binary);
		if (!out)
		{
			Fail("Could not write the extracted source to " + target.string() + ".");
			archive_read_free(reader);
			return false;
		}

		la_ssize_t read = 0;
		while ((read = archive_read_data(reader, buffer.data(), buffer.size())) > 0)
			out.write(buffer.data(), read);

		files += 1.0f;
		if (std::fmod(files, 50.0f) < 1.0f)
			SetMessage("Unpacking the source (" + std::to_string(int(files)) + " files)...");
	}

	archive_read_free(reader);
	sourceDir = SingleRoot(dest);
	progress = 1.0f;
	return true;
}

bool UpdateBuilder::PrepareToolchain()
{
	cmakePath = FindTool(L"cmake");
	if (cmakePath.empty())
	{
		const std::filesystem::path bundled =
			std::filesystem::path(L"C:\\Program Files\\CMake\\bin\\cmake.exe");
		if (std::filesystem::exists(bundled))
			cmakePath = bundled;
	}

	gitPath = FindTool(L"git");

	std::wstring programFilesX86;
	{
		std::array<wchar_t, MAX_PATH> value{};
		const DWORD length = GetEnvironmentVariableW(L"ProgramFiles(x86)", value.data(), DWORD(value.size()));
		if (length > 0 && length < value.size())
			programFilesX86 = value.data();
	}

	const std::filesystem::path vswhere = std::filesystem::path(programFilesX86) /
		L"Microsoft Visual Studio" / L"Installer" / L"vswhere.exe";

	if (!std::filesystem::exists(vswhere))
	{
		Fail("Visual Studio was not found (vswhere is missing). Install Visual Studio or the "
			"Build Tools with the \"Desktop development with C++\" workload, then try again.");
		return false;
	}

	const std::filesystem::path probeLog = workRoot / "vswhere.txt";
	const std::wstring probe = L"\"" + vswhere.wstring() + L"\" -latest -products * -requires "
		L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath";

	const std::atomic<bool> noCancel{ false };
	if (RunHidden(probe, probeLog, noCancel) != 0)
	{
		Fail("Visual Studio's C++ tools were not found. Install the \"Desktop development with C++\" "
			"workload, then try again.");
		return false;
	}

	const std::string installPath = FirstNonEmptyLine(ReadLogTail(probeLog, 8192));
	if (installPath.empty())
	{
		Fail("Visual Studio is installed, but without the C++ build tools. Add the "
			"\"Desktop development with C++\" workload, then try again.");
		return false;
	}

	const std::filesystem::path vsRoot = std::filesystem::u8path(installPath);
	vcvarsPath = vsRoot / L"VC" / L"Auxiliary" / L"Build" / L"vcvars64.bat";
	if (!std::filesystem::exists(vcvarsPath))
	{
		Fail("The Visual Studio C++ environment script (vcvars64.bat) is missing. Repair the "
			"Visual Studio installation, then try again.");
		return false;
	}

	// Ninja reports "[12/345]" counters, which drive the progress bar, and it does not care which
	// Visual Studio version is installed. It ships with Visual Studio and some package managers.
	std::filesystem::path ninja = FindTool(L"ninja");
	if (ninja.empty())
	{
		const std::filesystem::path bundledNinja = vsRoot / L"Common7" / L"IDE" / L"CommonExtensions" /
			L"Microsoft" / L"CMake" / L"Ninja" / L"ninja.exe";
		if (std::filesystem::exists(bundledNinja))
			ninja = bundledNinja;
	}

	useNinja = !ninja.empty();
	if (useNinja)
	{
		ninjaPath = ninja;
		generator = "Ninja";
	}
	else
	{
		// No Ninja: the Visual Studio generator is named after the installed version.
		const std::filesystem::path versionLog = workRoot / "vswhere-version.txt";
		const std::wstring versionProbe = L"\"" + vswhere.wstring() +
			L"\" -latest -products * -property installationVersion";

		std::string version;
		if (RunHidden(versionProbe, versionLog, noCancel) == 0)
			version = FirstNonEmptyLine(ReadLogTail(versionLog, 4096));

		if (version.rfind("18.", 0) == 0)
			generator = "Visual Studio 18 2026";
		else if (version.rfind("17.", 0) == 0)
			generator = "Visual Studio 17 2022";
		else if (version.rfind("16.", 0) == 0)
			generator = "Visual Studio 16 2019";
		else
		{
			Fail("Neither Ninja nor a Visual Studio version CMake knows how to drive was found. "
				"Install Ninja (or CMake's Ninja support) and try again.");
			return false;
		}
	}

	if (cmakePath.empty())
	{
		Fail("CMake was not found. Install CMake, or the \"C++ CMake tools for Windows\" component "
			"of Visual Studio, then try again.");
		return false;
	}

	if (gitPath.empty())
	{
		// A GUI launch does not always inherit the PATH a terminal would, so the usual install
		// folders are checked as well.
		auto probeGit = [](const std::filesystem::path& root) -> std::filesystem::path
		{
			for (const wchar_t* relative : { L"Git\\cmd\\git.exe", L"Git\\bin\\git.exe" })
			{
				const std::filesystem::path candidate = root / relative;
				if (std::filesystem::exists(candidate))
					return candidate;
			}

			return {};
		};

		std::vector<std::filesystem::path> roots;
		for (const wchar_t* variable : { L"ProgramFiles", L"ProgramFiles(x86)", L"LocalAppData" })
		{
			std::array<wchar_t, MAX_PATH> value{};
			const DWORD length = GetEnvironmentVariableW(variable, value.data(), DWORD(value.size()));
			if (length > 0 && length < value.size())
				roots.emplace_back(value.data());
		}

		for (const std::filesystem::path& root : roots)
		{
			gitPath = probeGit(root);
			if (!gitPath.empty())
				break;
		}
	}

	if (gitPath.empty())
	{
		Fail("Git was not found. The build downloads its dependencies with Git, so install Git and "
			"try again.");
		return false;
	}

	return true;
}

bool UpdateBuilder::Configure()
{
	const std::filesystem::path script = workRoot / "configure.bat";
	const std::filesystem::path log = workRoot / "configure.log";

	std::ostringstream text;
	text << "@echo off\r\n";
	text << "call \"" << vcvarsPath.string() << "\" >nul\r\n";
	text << "if errorlevel 1 exit /b 10\r\n";
	text << "set \"PATH=" << gitPath.parent_path().string() << ";" << ninjaPath.parent_path().string()
		<< ";%PATH%\"\r\n";
	text << "\"" << cmakePath.string() << "\" -S \"" << sourceDir.string() << "\" -B \""
		<< buildDir.string() << "\" -G \"" << generator << "\"";
	if (!useNinja)
		text << " -A x64";
	text << " -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5\r\n";
	text << "exit /b %ERRORLEVEL%\r\n";

	{
		std::ofstream out(script, std::ios::binary);
		out << text.str();
	}

	const int result = RunHidden(L"cmd.exe /c \"" + script.wstring() + L"\"", log, cancelRequested);
	if (result == -2)
		return false;

	if (result != 0)
	{
		std::string tail = ReadLogTail(log, 2000);
		if (result == 10)
			Fail("The Visual Studio build environment could not be started.");
		else
			Fail("Configuring the build failed. The last lines of the log:\n\n" + tail);
		return false;
	}

	return true;
}

bool UpdateBuilder::Compile()
{
	const std::filesystem::path script = workRoot / "build.bat";
	const std::filesystem::path log = workRoot / "build.log";

	{
		std::lock_guard lock(mutex);
		logPath = log.string();
	}

	std::ostringstream text;
	text << "@echo off\r\n";
	text << "call \"" << vcvarsPath.string() << "\" >nul\r\n";
	text << "if errorlevel 1 exit /b 10\r\n";
	text << "set \"PATH=" << gitPath.parent_path().string() << ";" << ninjaPath.parent_path().string()
		<< ";%PATH%\"\r\n";
	text << "\"" << cmakePath.string() << "\" --build \"" << buildDir.string()
		<< "\" --target Comet --config Release --parallel\r\n";
	text << "exit /b %ERRORLEVEL%\r\n";

	{
		std::ofstream out(script, std::ios::binary);
		out << text.str();
	}

	// Ninja prints "[done/total]" for every step; turn the newest one into a progress bar.
	auto poll = [this, &log]()
	{
		const std::string tail = ReadLogTail(log, 4096);
		const std::size_t close = tail.rfind(']');
		if (close == std::string::npos)
			return;

		const std::size_t open = tail.rfind('[', close);
		if (open == std::string::npos)
			return;

		int done = 0;
		int total = 0;
		if (std::sscanf(tail.c_str() + open, "[%d/%d]", &done, &total) == 2 && total > 0)
		{
			progress = float(double(done) / double(total));
			SetMessage("Compiling Comet... " + std::to_string(done) + " of " + std::to_string(total));
		}
	};

	const int result = RunHidden(L"cmd.exe /c \"" + script.wstring() + L"\"", log, cancelRequested, poll);
	if (result == -2)
		return false;

	if (result != 0)
	{
		// Compiler errors are the useful part, so surface the end of the log instead of a generic
		// message.
		std::string tail = ReadLogTail(log, 2500);
		Fail("Compiling failed. The last lines of the build log:\n\n" + tail);
		return false;
	}

	std::error_code ec;
	std::filesystem::path found;
	std::filesystem::file_time_type newest{};
	for (const auto& entry : std::filesystem::recursive_directory_iterator(buildDir, ec))
	{
		if (!entry.is_regular_file(ec))
			continue;

		if (entry.path().filename() != EXE_NAME)
			continue;

		const auto time = entry.last_write_time(ec);
		if (found.empty() || time > newest)
		{
			found = entry.path();
			newest = time;
		}
	}

	if (found.empty())
	{
		Fail("The build finished, but no Comet.exe was produced.");
		return false;
	}

	builtExe = found;
	progress = 1.0f;
	return true;
}

bool UpdateBuilder::Install()
{
	const std::filesystem::path exe = GetRunningExePath();
	if (exe.empty())
	{
		Fail("Could not locate the running Comet.exe.");
		return false;
	}

	const std::filesystem::path staged(exe.wstring() + L".new");
	const std::filesystem::path backup(exe.wstring() + L".old");

	std::error_code ec;
	std::filesystem::remove(staged, ec);
	std::filesystem::remove(backup, ec);

	std::filesystem::copy_file(builtExe, staged, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec)
	{
		Fail("Could not copy the new build next to Comet.exe (" + ec.message() + "). Is the folder "
			"writable?");
		return false;
	}

	// Windows allows renaming a running image, which is what makes an in-place update possible.
	std::filesystem::rename(exe, backup, ec);
	if (ec)
	{
		std::filesystem::remove(staged, ec);
		Fail("Could not move the current Comet.exe aside (" + ec.message() + ").");
		return false;
	}

	std::filesystem::rename(staged, exe, ec);
	if (ec)
	{
		std::error_code restore;
		std::filesystem::rename(backup, exe, restore);
		std::filesystem::remove(staged, restore);
		Fail("Could not put the new Comet.exe in place (" + ec.message() + ").");
		return false;
	}

	return true;
}

void UpdateBuilder::CleanWorkFiles()
{
	std::error_code ec;
	// Only the exe was wanted; the source, the build tree and the archive go away. The logs stay
	// in %TEMP%\CometUpdate for troubleshooting.
	std::filesystem::remove_all(sourceDir, ec);
	std::filesystem::remove_all(buildDir, ec);
	// The GitHub zip unpacks into a folder inside src, so the wrapper goes too and nothing but the
	// logs is left behind.
	std::filesystem::remove_all(workRoot / "src", ec);
	std::filesystem::remove(workRoot / "source.zip", ec);
	std::filesystem::remove(workRoot / "configure.bat", ec);
	std::filesystem::remove(workRoot / "build.bat", ec);
	std::filesystem::remove(workRoot / "vswhere.txt", ec);
	std::filesystem::remove(workRoot / "vswhere-version.txt", ec);
}
