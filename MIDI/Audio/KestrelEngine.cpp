#ifdef _WIN32

#include "KestrelEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <imgui.h>
#include "Utils.h"

namespace
{
	constexpr int SAMPLE_RATE = 48000;

	// How far playback may drift from the timer before it is pulled back. Kestrel's WAV is
	// sample-accurate, so a seek this small is inaudible; a larger tolerance lets a slow disk run
	// the audio behind the notes.
	constexpr double MAX_DRIFT_SECONDS = 0.15;

	const int BLOCK_SIZES[] = { 512, 1024, 2048, 4096, 8192 };
	const char* const LIMITERS[] = { "Gentle", "Balanced", "Loud" };
	const char* const GPU_BACKENDS[] = { "Automatic (the strongest)", "Vulkan", "DX12", "Metal", "GL" };
	const char* const INTERPOLATIONS[] = { "Nearest", "Linear", "Cubic" };

	std::wstring Widen(const std::string& text)
	{
		if (text.empty()) return std::wstring();

		const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
		if (size <= 0) return std::wstring();

		std::wstring wide((size_t)size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wide.data(), size);
		return wide;
	}

	uint64_t Fnv1a(const std::string& text)
	{
		uint64_t hash = 1469598103934665603ull;
		for (unsigned char c : text)
		{
			hash ^= (uint64_t)c;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	std::string Hex(uint64_t value)
	{
		char buffer[17];
		std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)value);
		return buffer;
	}

	// kestrel.exe is looked for beside the app first, so a copy dropped into the install folder is
	// simply there; PATH is the fallback for a normal kestrel installation.
	std::string PartialOf(const std::string& wavPath)
	{
		if (wavPath.empty()) return std::string();

		const std::filesystem::path path(wavPath);
		return (path.parent_path() / (path.stem().string() + ".partial" + path.extension().string())).string();
	}

	// Delete sharing lets kestrel rename or replace the file while the audio is being read.
	HANDLE OpenSharedRead(const std::string& path)
	{
		return CreateFileW(Widen(path).c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	std::string FindKestrelBinary()
	{
		const char* const candidates[] = { "./kestrel/kestrel.exe", "./kestrel.exe", "kestrel.exe" };

		std::error_code error;
		for (const char* candidate : candidates)
		{
			if (std::filesystem::exists(candidate, error)) return candidate;
		}

		return "kestrel.exe";
	}

	std::string ReadTail(const std::string& path, size_t maxBytes)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open()) return std::string();

		stream.seekg(0, std::ios::end);
		const std::streamoff size = stream.tellg();
		const std::streamoff start = size > (std::streamoff)maxBytes ? size - (std::streamoff)maxBytes : 0;

		stream.seekg(start, std::ios::beg);
		std::string text((size_t)(size - start), '\0');
		stream.read(text.data(), (std::streamsize)text.size());
		text.resize((size_t)stream.gcount());
		return text;
	}

	// Runs kestrel.exe to completion with its console output in `logPath`, so a long render does not
	// block the app and the reason for a failure can be read back afterwards.
	bool RunProcess(const std::wstring& commandLine, const std::wstring& workingDirectory,
		const std::wstring& logPath, std::atomic_bool& cancel, DWORD& exitCode, std::string& error)
	{
		SECURITY_ATTRIBUTES attributes{};
		attributes.nLength = sizeof(attributes);
		attributes.bInheritHandle = TRUE;

		HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);

		if (log != INVALID_HANDLE_VALUE)
		{
			startup.dwFlags |= STARTF_USESTDHANDLES;
			startup.hStdOutput = log;
			startup.hStdError = log;
			startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		}

		PROCESS_INFORMATION process{};
		std::wstring mutableCommandLine = commandLine;

		if (!CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr,
			log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process))
		{
			const DWORD lastError = GetLastError();
			if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

			error = "kestrel.exe could not be started (Windows error " + std::to_string(lastError) +
				"). Put kestrel.exe beside Comet, or point the engine at it below.";
			return false;
		}

		for (;;)
		{
			if (cancel.load())
			{
				TerminateProcess(process.hProcess, 1);
				break;
			}

			if (WaitForSingleObject(process.hProcess, 200) == WAIT_OBJECT_0) break;
		}

		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);

		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		return true;
	}
}

KestrelEngine::KestrelEngine(Mode mode) : mode(mode)
{
	kestrelPath = FindKestrelBinary();
	renderLogPath = "./kestrel_cache/render.log";

	if (mode != Mode::Gpu) settings.blockSize = 4096;
	if (mode == Mode::Gpu) settings.maxVoices = 1000000;
}

KestrelEngine::~KestrelEngine()
{
	Destroy();
}

bool KestrelEngine::UsesMerlin() const
{
	if (mode == Mode::Merlin) return true;
	return mode == Mode::RenderedAhead && settings.engineForAhead == 0;
}

std::string KestrelEngine::GetName()
{
	switch (mode)
	{
	case Mode::Merlin: return "Merlin (GPU)";
	case Mode::RenderedAhead: return "Rendered ahead";
	default: return "Kestrel (GPU, Experimental)";
	}
}

std::string KestrelEngine::GetSerializationKey()
{
	switch (mode)
	{
	case Mode::Merlin: return "merlin";
	case Mode::RenderedAhead: return "renderedAhead";
	default: return "kestrel";
	}
}

void KestrelEngine::Initialize()
{
	if (bassReady) return;

	BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 10);
	BASS_SetConfig(BASS_CONFIG_BUFFER, 50);

	if (!BASS_Init(-1, SAMPLE_RATE, 0, 0, nullptr))
	{
		SetStatus("BASS could not open an output device (error " +
			std::to_string(BASS_ErrorGetCode()) + "). Kestrel's render still works.");
		return;
	}

	bassReady = true;
}

void KestrelEngine::Destroy()
{
	Stop();

	renderCancel = true;
	if (renderThread.joinable()) renderThread.join();

	CloseChannel();

	{
		std::lock_guard<std::mutex> lock(pcmMutex);
		if (pcmHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pcmHandle);
			pcmHandle = INVALID_HANDLE_VALUE;
		}

		pcmReady = false;
	}

	if (bassReady)
	{
		BASS_Free();
		bassReady = false;
	}
}

void KestrelEngine::SetSourcePath(const std::string& path)
{
	if (sourcePath == path) return;

	// A different song means a different render; whatever was playing belongs to the old one.
	CloseChannel();
	cachedWavPath.clear();
	previewPath.clear();
	streamPath.clear();
	renderedAudioSeconds.store(0.0);
	previewFinished.store(false);
	renderFinished.store(false);

	{
		std::lock_guard<std::mutex> lock(pcmMutex);
		if (pcmHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(pcmHandle);
			pcmHandle = INVALID_HANDLE_VALUE;
		}

		pcmReady = false;
		pcmFramesAvailable.store(0);
	}

	sourcePath = path;

	try
	{
		std::filesystem::create_directories("./kestrel_cache");
	}
	catch (...)
	{
	}

	// "Make ahead while stopped" is the point of the rendered-ahead engine: the render starts as
	// soon as the song is loaded rather than waiting for the play button.
	if (mode == Mode::RenderedAhead && settings.makeAheadWhileStopped && !sourcePath.empty())
	{
		std::string wavPath, error;
		EnsureRendered(wavPath, error);
	}
}

void KestrelEngine::Start(std::shared_ptr<MIDISequence> seq, std::shared_ptr<MIDITimer> timer)
{
	Stop();

	if (!bassReady) Initialize();
	if (!bassReady) return;

	stopFlag = false;
	playing = true;

	std::string wavPath, error;
	if (EnsureRendered(wavPath, error))
	{
		// Rendered ahead plays the file as it grows, so it never opens the finished WAV directly;
		// its sync loop picks up whatever audio exists.
		if (!Streaming()) OpenChannel(wavPath);
	}
	else if (!error.empty())
	{
		SetStatus(error);
	}

	syncThread = std::thread(&KestrelEngine::SyncLoop, this, timer);
}

void KestrelEngine::Stop()
{
	stopFlag = true;
	if (syncThread.joinable()) syncThread.join();

	CloseChannel();
	playing = false;
}

void KestrelEngine::Reset()
{
	if (channel != 0)
	{
		BASS_ChannelSetPosition(channel, 0, BASS_POS_BYTE);
		BASS_ChannelPause(channel);
	}
}

void KestrelEngine::Mute()
{
	muted = true;
	if (channel != 0) BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, 0.0f);
}

void KestrelEngine::Unmute()
{
	muted = false;
	if (channel != 0) BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, 1.0f);
}

bool KestrelEngine::IsPlaying()
{
	return playing.load();
}

std::string KestrelEngine::CachedWavPath() const
{
	std::ostringstream key;
	key << sourcePath;

	std::error_code error;
	const auto size = std::filesystem::file_size(sourcePath, error);
	if (!error) key << "|" << size;

	const auto writeTime = std::filesystem::last_write_time(sourcePath, error);
	if (!error) key << "|" << writeTime.time_since_epoch().count();

	key << "|" << (UsesMerlin() ? "merlin" : "gpu")
		<< "|" << settings.maxVoices
		<< "|" << settings.blockSize
		<< "|" << settings.gpuBackend
		<< "|" << settings.interpolation
		<< "|" << settings.velocityThreshold;

	// Only the soundfonts the render would actually use, so switching Merlin's one font back and
	// forth does not pile up renders of the same audio.
	const size_t used = UsesMerlin() ? std::min<size_t>(1, settings.soundfonts.size()) : settings.soundfonts.size();
	for (size_t i = 0; i < used; i++)
		key << "|" << settings.soundfonts[i];

	return "./kestrel_cache/" + Hex(Fnv1a(key.str())) + ".wav";
}

bool KestrelEngine::EnsureRendered(std::string& wavPath, std::string& error)
{
	if (sourcePath.empty())
	{
		error = "Load a MIDI first: Kestrel renders the file itself rather than a stream of notes.";
		return false;
	}

	if (cachedWavPath.empty()) cachedWavPath = CachedWavPath();

	if (std::filesystem::exists(cachedWavPath))
	{
		wavPath = cachedWavPath;
		return true;
	}

	if (renderActive.load())
	{
		error = "The render is still running; playback starts when it finishes.";
		return false;
	}

	if (renderThread.joinable()) renderThread.join();

	renderCancel = false;
	renderActive = true;

	if (Streaming())
	{
		const std::filesystem::path out(cachedWavPath);
		previewPath = (out.parent_path() / (out.stem().string() + ".preview" + out.extension().string())).string();

		std::error_code errorCode;
		std::filesystem::remove(previewPath, errorCode);

		renderedAudioSeconds.store(0.0);
		previewFinished.store(false);
		renderFinished.store(false);

		renderThread = std::thread(&KestrelEngine::RenderLoopStreaming, this);
		error = "Rendering ahead on the GPU. Audio plays as it is written; the log is " + renderLogPath + ".";
	}
	else
	{
		renderThread = std::thread(&KestrelEngine::RenderLoop, this);
		error = "Rendering on the GPU. Playback starts when the render finishes; the log is " + renderLogPath + ".";
	}

	return false;
}

void KestrelEngine::RenderLoop()
{
	std::string wavPath = cachedWavPath;

	std::error_code errorCode;
	std::filesystem::create_directories("./kestrel_cache", errorCode);

	const std::filesystem::path kestrel(kestrelPath);
	const std::wstring workingDirectory =
		kestrel.has_parent_path() ? kestrel.parent_path().wstring() : std::wstring();

	const size_t usedFonts = UsesMerlin()
		? std::min<size_t>(1, settings.soundfonts.size())
		: settings.soundfonts.size();

	const int attempts = settings.lowerVoicesWhenNeeded ? 4 : 1;
	int voices = std::max(1, settings.maxVoices);
	DWORD exitCode = 0;

	for (int attempt = 0; attempt < attempts; attempt++)
	{
		std::wstring commandLine = L"\"" + Widen(kestrelPath) + L"\" --force-cli render \"" +
			Widen(sourcePath) + L"\" -o \"" + Widen(wavPath) + L"\"";

		// kestrel layers -s in order, each overriding the ones above it - exactly the list order
		// the panel describes.
		for (size_t i = 0; i < usedFonts; i++)
			commandLine += L" -s \"" + Widen(settings.soundfonts[i]) + L"\"";

		commandLine += L" --max-voices " + std::to_wstring(voices);
		commandLine += L" --block " + std::to_wstring(settings.blockSize);

		if (settings.gpuBackend > 0 && settings.gpuBackend <= 4)
		{
			static const wchar_t* const names[] = { L"", L"vulkan", L"dx12", L"metal", L"gl" };
			commandLine += std::wstring(L" --gpu-backend ") + names[settings.gpuBackend];
		}

		if (settings.interpolation > 0 && settings.interpolation <= 2)
		{
			static const wchar_t* const names[] = { L"", L"linear", L"cubic" };
			commandLine += std::wstring(L" --interp ") + names[settings.interpolation];
		}

		if (settings.velocityThreshold > 0)
			commandLine += L" --min-velocity " + std::to_wstring(settings.velocityThreshold);

		if (UsesMerlin()) commandLine += L" --backend merlin";

		SetStatus((UsesMerlin() ? "Merlin is rendering every note" : "Kestrel is rendering on the GPU") +
			std::string(" with ") + std::to_string(voices) + " voices. Log: " + renderLogPath);

		std::string error;
		if (!RunProcess(commandLine, workingDirectory, Widen(renderLogPath), renderCancel, exitCode, error))
		{
			SetStatus(error);
			renderActive = false;
			return;
		}

		if (exitCode == 0) break;

		// "Lower it by itself when it can't keep up": a render that ran out of video memory is
		// retried with half the voices, which is what makes a too-large max work on a smaller card.
		const std::string log = ReadTail(renderLogPath, 64 * 1024);
		const bool outOfMemory = log.find("memory") != std::string::npos
			|| log.find("VRAM") != std::string::npos
			|| log.find("alloc") != std::string::npos
			|| log.find("pool") != std::string::npos;

		if (!settings.lowerVoicesWhenNeeded || !outOfMemory || voices <= 4096) break;

		voices = std::max(4096, voices / 2);
		SetStatus("Kestrel ran out of video memory; retrying with " + std::to_string(voices) + " voices.");
	}

	if (exitCode == 0 && std::filesystem::exists(wavPath))
	{
		SetStatus("The render is ready (" +
			std::to_string((long long)(std::filesystem::file_size(wavPath, errorCode) / (1024 * 1024))) +
			" MB).");
	}
	else
	{
		// A half-written WAV would play as a truncated song, so it is not kept.
		std::filesystem::remove(wavPath, errorCode);
		SetStatus("The render failed with exit code " + std::to_string(exitCode) + "; see " + renderLogPath + ".");
	}

	renderActive = false;
}

// One kestrel run, with its JSON progress on stdout so the engine knows how much audio exists
// before the file is whole. `secondsLimit` 0 renders the whole song; a positive value stops after
// that many seconds of audio. --no-resume writes the audio straight into the output file instead
// of a ".partial" renamed at the end, so the file being played never moves and no large checkpoint
// is left beside it.
bool KestrelEngine::RenderTo(const std::string& outputPath, int secondsLimit)
{
	std::error_code errorCode;
	std::filesystem::create_directories("./kestrel_cache", errorCode);

	const std::filesystem::path kestrel(kestrelPath);
	const std::wstring workingDirectory =
		kestrel.has_parent_path() ? kestrel.parent_path().wstring() : std::wstring();

	const size_t usedFonts = UsesMerlin()
		? std::min<size_t>(1, settings.soundfonts.size())
		: settings.soundfonts.size();

	std::wstring commandLine = L"\"" + Widen(kestrelPath) + L"\" --force-cli render \"" +
		Widen(sourcePath) + L"\" -o \"" + Widen(outputPath) + L"\" --progress json --no-resume";

	if (secondsLimit > 0)
		commandLine += L" --seconds " + std::to_wstring(secondsLimit);

	for (size_t i = 0; i < usedFonts; i++)
		commandLine += L" -s \"" + Widen(settings.soundfonts[i]) + L"\"";

	commandLine += L" --max-voices " + std::to_wstring(std::max(1, settings.maxVoices));
	commandLine += L" --block " + std::to_wstring(settings.blockSize);

	if (settings.gpuBackend > 0 && settings.gpuBackend <= 4)
	{
		static const wchar_t* const names[] = { L"", L"vulkan", L"dx12", L"metal", L"gl" };
		commandLine += std::wstring(L" --gpu-backend ") + names[settings.gpuBackend];
	}

	if (settings.interpolation > 0 && settings.interpolation <= 2)
	{
		static const wchar_t* const names[] = { L"", L"linear", L"cubic" };
		commandLine += std::wstring(L" --interp ") + names[settings.interpolation];
	}

	if (settings.velocityThreshold > 0)
		commandLine += L" --min-velocity " + std::to_wstring(settings.velocityThreshold);

	if (UsesMerlin()) commandLine += L" --backend merlin";

	SECURITY_ATTRIBUTES attributes{};
	attributes.nLength = sizeof(attributes);
	attributes.bInheritHandle = TRUE;

	HANDLE readEnd = nullptr;
	HANDLE writeEnd = nullptr;

	if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0))
	{
		SetStatus("Could not create the pipe kestrel's progress needs.");
		return false;
	}

	SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

	HANDLE log = CreateFileW(Widen(renderLogPath).c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags |= STARTF_USESTDHANDLES;
	startup.hStdOutput = writeEnd;
	startup.hStdError = log != INVALID_HANDLE_VALUE ? log : writeEnd;
	startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION process{};
	std::wstring mutableCommandLine = commandLine;

	const BOOL started = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
		&startup, &process);

	CloseHandle(writeEnd);

	if (!started)
	{
		const DWORD lastError = GetLastError();
		CloseHandle(readEnd);
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

		SetStatus("kestrel.exe could not be started (Windows error " + std::to_string(lastError) + ").");
		return false;
	}

	SetStatus(UsesMerlin()
		? "Merlin is rendering every note; its audio appears once the keys are mixed."
		: "Rendering ahead on the GPU; audio plays as it is written.");

	std::string pending;
	char chunk[4096];

	for (;;)
	{
		DWORD available = 0;
		if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) break;

		if (available > 0)
		{
			DWORD read = 0;
			if (!ReadFile(readEnd, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;

			pending.append(chunk, read);

			size_t newline = 0;
			while ((newline = pending.find('\n')) != std::string::npos)
			{
				const std::string line = pending.substr(0, newline);
				pending.erase(0, newline + 1);

				double audioSeconds = 0.0;
				if (RenderAhead::ParseAudioSeconds(line, audioSeconds))
					renderedAudioSeconds.store(audioSeconds);
			}

			continue;
		}

		if (renderCancel.load())
		{
			TerminateProcess(process.hProcess, 1);
			break;
		}

		if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) break;

		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	DWORD exitCode = 0;
	GetExitCodeProcess(process.hProcess, &exitCode);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(readEnd);
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);

	if (exitCode != 0)
		SetStatus("The render stopped with exit code " + std::to_string(exitCode) + "; see " + renderLogPath + ".");

	return exitCode == 0;
}

// Rendered ahead renders two files: a short first chunk, so playback can start after a few
// seconds instead of after the whole song, and then the whole song behind the playback.
void KestrelEngine::RenderLoopStreaming()
{
	renderedAudioSeconds.store(0.0);
	SetStatus("Rendering the first " + std::to_string(settings.renderAheadSeconds) + " seconds...");

	const bool previewRendered = RenderTo(previewPath, settings.renderAheadSeconds);
	previewFinished.store(true);

	if (renderCancel.load())
	{
		std::error_code errorCode;
		std::filesystem::remove(previewPath, errorCode);
		std::filesystem::remove(cachedWavPath, errorCode);

		renderActive = false;
		renderFinished.store(true);
		SetStatus("The render was cancelled.");
		return;
	}

	// A song that ended inside the chunk is complete already, so the second render is skipped.
	if (previewRendered && renderedAudioSeconds.load() + 0.5 < (double)settings.renderAheadSeconds)
	{
		std::error_code errorCode;
		std::filesystem::copy_file(previewPath, cachedWavPath,
			std::filesystem::copy_options::overwrite_existing, errorCode);
		std::filesystem::remove(previewPath, errorCode);

		renderActive = false;
		renderFinished.store(true);
		SetStatus("The whole song is rendered and cached for the next play.");
		return;
	}

	SetStatus("Playing the first chunk; the rest of the song renders behind it...");

	// The rest of the song, into the file playback takes over from when the chunk runs out.
	renderedAudioSeconds.store(0.0);
	const bool fullRendered = RenderTo(cachedWavPath, 0);

	renderActive = false;
	renderFinished.store(true);

	if (fullRendered)
	{
		SetStatus("The whole song is rendered and cached for the next play.");
	}
	else if (renderCancel.load())
	{
		std::error_code errorCode;
		std::filesystem::remove(cachedWavPath, errorCode);
		SetStatus("The rest of the render was cancelled; the first chunk still plays.");
	}
	else
	{
		std::error_code errorCode;
		std::filesystem::remove(cachedWavPath, errorCode);
		SetStatus("The rest of the render failed; see " + renderLogPath + ".");
	}
}

// A finished render is best, then the file the full render is writing, then the first chunk.
bool KestrelEngine::OpenGrowingWav()
{
	if (previewPath.empty() && cachedWavPath.empty()) return false;

	const std::string candidates[] = { cachedWavPath, PartialOf(cachedWavPath), previewPath,
		PartialOf(previewPath) };

	for (const std::string& path : candidates)
	{
		if (OpenStream(path, 0.0, false)) return true;
	}

	return false;
}

bool KestrelEngine::OpenStream(const std::string& path, double startSeconds, bool requireAudio)
{
	if (path.empty()) return false;

	HANDLE handle = OpenSharedRead(path);
	if (handle == INVALID_HANDLE_VALUE) return false;

	LARGE_INTEGER sizeValue{};
	if (!GetFileSizeEx(handle, &sizeValue) || sizeValue.QuadPart < 64)
	{
		CloseHandle(handle);
		return false;
	}

	std::vector<uint8_t> head((size_t)std::min<LONGLONG>(sizeValue.QuadPart, 8192));

	LARGE_INTEGER beginning{};
	SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN);

	DWORD read = 0;
	if (!ReadFile(handle, head.data(), (DWORD)head.size(), &read, nullptr))
	{
		CloseHandle(handle);
		return false;
	}
	head.resize(read);

	RenderAhead::WavInfo info;
	if (!RenderAhead::ParseWavHeader(head.data(), head.size(), info))
	{
		CloseHandle(handle);
		return false;
	}

	// The playback callback feeds BASS 32-bit float frames, which is kestrel's WAV default.
	if (info.bits != 32 || info.channels == 0 || info.rate == 0)
	{
		CloseHandle(handle);
		return false;
	}

	const uint64_t frames = RenderAhead::AvailableFrames((uint64_t)sizeValue.QuadPart, info);
	const uint64_t startFrame = (uint64_t)(std::max(0.0, startSeconds) * (double)info.rate);

	if (requireAudio && frames <= startFrame)
	{
		CloseHandle(handle);
		return false;
	}

	std::lock_guard<std::mutex> lock(pcmMutex);

	if (pcmHandle != INVALID_HANDLE_VALUE)
		CloseHandle(pcmHandle);

	pcmHandle = handle;
	pcmInfo = info;
	pcmBytesPerFrame = RenderAhead::BytesPerFrame(info);
	pcmCursorFrames = startFrame;
	pcmFramesAvailable.store(frames);
	streamPath = path;
	pcmReady = true;
	return true;
}

bool KestrelEngine::SwitchStream(const std::string& path, double startSeconds)
{
	const std::string candidates[] = { path, PartialOf(path) };

	for (const std::string& candidate : candidates)
	{
		if (!OpenStream(candidate, startSeconds, true)) continue;

		// BASS still has audio queued from the chunk; rebuilding the stream makes the switch
		// happen at the playhead instead of once that queue has drained.
		CreatePushStream();
		return true;
	}

	return false;
}

void KestrelEngine::RefreshAvailableFrames()
{
	std::lock_guard<std::mutex> lock(pcmMutex);
	if (!pcmReady || pcmHandle == INVALID_HANDLE_VALUE) return;

	LARGE_INTEGER size{};
	if (!GetFileSizeEx(pcmHandle, &size))
	{
		pcmFramesAvailable.store(0);
		return;
	}

	pcmFramesAvailable.store(RenderAhead::AvailableFrames((uint64_t)size.QuadPart, pcmInfo));
}

DWORD CALLBACK KestrelEngine::PcmProc(HSTREAM, void* buffer, DWORD lengthBytes, void* user)
{
	auto* self = static_cast<KestrelEngine*>(user);

	std::memset(buffer, 0, lengthBytes);
	if (self->pcmBytesPerFrame == 0) return lengthBytes;

	const uint64_t wantedFrames = lengthBytes / self->pcmBytesPerFrame;

	std::lock_guard<std::mutex> lock(self->pcmMutex);
	if (self->pcmHandle == INVALID_HANDLE_VALUE) return lengthBytes;

	const uint64_t available = self->pcmFramesAvailable.load(std::memory_order_relaxed);
	const uint64_t start = self->pcmCursorFrames;

	// Only frames that really exist are read. A gap is silence rather than a repeat, and the sync
	// loop pulls the playhead back once the render has written past it.
	if (start < available)
	{
		const uint64_t frames = std::min(wantedFrames, available - start);

		LARGE_INTEGER offset{};
		offset.QuadPart = (LONGLONG)(self->pcmInfo.dataOffset + start * self->pcmBytesPerFrame);

		DWORD read = 0;
		if (SetFilePointerEx(self->pcmHandle, offset, nullptr, FILE_BEGIN))
			ReadFile(self->pcmHandle, buffer, (DWORD)(frames * self->pcmBytesPerFrame), &read, nullptr);
	}

	self->pcmCursorFrames = start + wantedFrames;
	return lengthBytes;
}

bool KestrelEngine::CreatePushStream()
{
	CloseChannel();

	channel = BASS_StreamCreate(pcmInfo.rate, pcmInfo.channels, BASS_SAMPLE_FLOAT, PcmProc, this);
	if (channel == 0)
	{
		SetStatus("BASS could not create the playback stream (error " +
			std::to_string(BASS_ErrorGetCode()) + ").");
		return false;
	}

	BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, muted.load() ? 0.0f : 1.0f);
	BASS_ChannelPlay(channel, FALSE);
	return true;
}

void KestrelEngine::SyncLoop(std::shared_ptr<MIDITimer> timer)
{
	while (!stopFlag.load())
	{
		if (Streaming())
		{
			if (!pcmReady && !OpenGrowingWav())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}

			RefreshAvailableFrames();

			const double wanted = timer ? std::max(0.0, timer->Elapsed()) : 0.0;
			const double ahead = (double)settings.renderAheadSeconds;
			const double available = (double)pcmFramesAvailable.load() / (double)pcmInfo.rate;

			// The first chunk only covers the start of the song. Once the playhead reaches its end,
			// the full render - which has been running behind the playback - becomes the source.
			if (streamPath == previewPath && wanted >= available - 0.02)
			{
				if (SwitchStream(cachedWavPath, wanted))
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(4));
					continue;
				}

				if (previewFinished.load() && renderFinished.load())
				{
					// Nothing more is coming, so stop here instead of playing silence.
					if (channel != 0) BASS_ChannelPause(channel);

					SetStatus("The rendered audio ends at " + std::to_string((int)available) +
						" s; the rest of the song could not be rendered.");

					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
			}

			// The point of the engine: do not start (or keep playing) without enough audio in front
			// of the playhead, but do not wait for the whole song either.
			const bool enoughAudio = renderFinished.load() || wanted + ahead <= available + 0.02;

			if (!enoughAudio)
			{
				if (channel != 0) BASS_ChannelPause(channel);

				SetStatus("Rendered ahead: " + std::to_string((int)available) + " s of audio ready, "
					"waiting for " + std::to_string((int)(wanted + ahead)) + " s.");

				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				continue;
			}

			if (channel == 0 && !CreatePushStream())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}

			BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, muted.load() ? 0.0f : 1.0f);

			if (!timer->IsRunning() || timer->IsPaused())
			{
				BASS_ChannelPause(channel);
			}
			else
			{
				const double audioPosition = (double)pcmCursorFrames / (double)pcmInfo.rate;

				if (std::fabs(audioPosition - wanted) > 0.25)
				{
					{
						std::lock_guard<std::mutex> lock(pcmMutex);
						pcmCursorFrames = (uint64_t)(wanted * pcmInfo.rate);
					}

					// BASS keeps a little audio queued, so a seek only takes effect at once if the
					// stream is rebuilt; that also throws away whatever silence was queued.
					CreatePushStream();
				}
				else if (BASS_ChannelIsActive(channel) != BASS_ACTIVE_PLAYING)
				{
					BASS_ChannelPlay(channel, FALSE);
				}
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(8));
			continue;
		}

		if (channel == 0 && !renderActive.load() && !cachedWavPath.empty() &&
			std::filesystem::exists(cachedWavPath))
		{
			OpenChannel(cachedWavPath);
		}

		if (channel != 0 && timer)
		{
			BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, muted.load() ? 0.0f : 1.0f);

			if (!timer->IsRunning() || timer->IsPaused())
			{
				BASS_ChannelPause(channel);
			}
			else
			{
				const double wanted = timer->Elapsed();
				const double position =
					BASS_ChannelBytes2Seconds(channel, BASS_ChannelGetPosition(channel, BASS_POS_BYTE));

				if (wanted >= 0.0 && std::fabs(position - wanted) > MAX_DRIFT_SECONDS)
				{
					BASS_ChannelSetPosition(channel, BASS_ChannelSeconds2Bytes(channel, wanted), BASS_POS_BYTE);
					BASS_ChannelPlay(channel, FALSE);
				}
				else if (BASS_ChannelIsActive(channel) != BASS_ACTIVE_PLAYING)
				{
					// Restarted after a pause, or the song ran past the end of the render.
					BASS_ChannelPlay(channel, FALSE);
				}
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(8));
	}
}

void KestrelEngine::OpenChannel(const std::string& wavPath)
{
	CloseChannel();

	channel = BASS_StreamCreateFile(FALSE, wavPath.c_str(), 0, 0, BASS_SAMPLE_FLOAT);
	if (channel == 0)
	{
		SetStatus("The rendered WAV could not be opened (BASS error " +
			std::to_string(BASS_ErrorGetCode()) + ").");
		return;
	}

	BASS_ChannelSetAttribute(channel, BASS_ATTRIB_VOL, muted.load() ? 0.0f : 1.0f);
	BASS_ChannelPlay(channel, FALSE);
	SetStatus("Playing the rendered audio.");
}

void KestrelEngine::CloseChannel()
{
	if (channel == 0) return;

	BASS_ChannelStop(channel);
	BASS_StreamFree(channel);
	channel = 0;
}

void KestrelEngine::SetStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	if (status == text) return;

	status = text;
	std::cout << "[kestrel] " << text << std::endl;
}

std::string KestrelEngine::GetStatus() const
{
	std::lock_guard<std::mutex> lock(statusMutex);
	return status;
}

YAML::Node KestrelEngine::GetSettings()
{
	YAML::Node node;
	YAML::Node fonts(YAML::NodeType::Sequence);
	for (const std::string& font : settings.soundfonts)
		fonts.push_back(font);

	node["soundfonts"] = fonts;
	node["maxVoices"] = settings.maxVoices;
	node["lowerVoicesWhenNeeded"] = settings.lowerVoicesWhenNeeded;
	node["slowDownToKeepUp"] = settings.slowDownToKeepUp;
	node["blockSize"] = settings.blockSize;
	node["limiter"] = settings.limiter;
	node["ceilingDb"] = settings.ceilingDb;
	node["dcBlocker"] = settings.dcBlocker;
	node["gpuBackend"] = settings.gpuBackend;
	node["interpolation"] = settings.interpolation;
	node["velocityThreshold"] = settings.velocityThreshold;
	node["engine"] = settings.engineForAhead;
	node["useKestrelWhereMerlinInexact"] = settings.useKestrelWhereMerlinInexact;
	node["renderAheadSeconds"] = settings.renderAheadSeconds;
	node["makeAheadWhileStopped"] = settings.makeAheadWhileStopped;
	node["kestrelPath"] = kestrelPath;
	return node;
}

void KestrelEngine::LoadSettings(const YAML::Node& node)
{
	if (!node) return;

	if (node["kestrelPath"]) kestrelPath = node["kestrelPath"].as<std::string>();

	if (node["soundfonts"] && node["soundfonts"].IsSequence())
	{
		settings.soundfonts.clear();
		for (const auto& entry : node["soundfonts"])
		{
			try
			{
				settings.soundfonts.push_back(entry.as<std::string>());
			}
			catch (...)
			{
			}
		}
	}

	if (node["maxVoices"]) settings.maxVoices = std::clamp(node["maxVoices"].as<int>(), 1, 16777216);
	if (node["lowerVoicesWhenNeeded"]) settings.lowerVoicesWhenNeeded = node["lowerVoicesWhenNeeded"].as<bool>();
	if (node["slowDownToKeepUp"]) settings.slowDownToKeepUp = node["slowDownToKeepUp"].as<bool>();
	if (node["blockSize"]) settings.blockSize = std::clamp(node["blockSize"].as<int>(), 512, 8192);
	if (node["limiter"]) settings.limiter = std::clamp(node["limiter"].as<int>(), 0, 2);
	if (node["ceilingDb"]) settings.ceilingDb = std::clamp(node["ceilingDb"].as<int>(), -30, 0);
	if (node["dcBlocker"]) settings.dcBlocker = node["dcBlocker"].as<bool>();
	if (node["gpuBackend"]) settings.gpuBackend = std::clamp(node["gpuBackend"].as<int>(), 0, 4);
	if (node["interpolation"]) settings.interpolation = std::clamp(node["interpolation"].as<int>(), 0, 2);
	if (node["velocityThreshold"]) settings.velocityThreshold = std::clamp(node["velocityThreshold"].as<int>(), 0, 127);
	if (node["engine"]) settings.engineForAhead = std::clamp(node["engine"].as<int>(), 0, 2);
	if (node["useKestrelWhereMerlinInexact"]) settings.useKestrelWhereMerlinInexact = node["useKestrelWhereMerlinInexact"].as<bool>();
	if (node["renderAheadSeconds"]) settings.renderAheadSeconds = std::clamp(node["renderAheadSeconds"].as<int>(), 1, 600);
	if (node["makeAheadWhileStopped"]) settings.makeAheadWhileStopped = node["makeAheadWhileStopped"].as<bool>();
}

namespace
{
	// The ordered soundfont list: additions go to the bottom, and order is what decides which font
	// overrides which, so it can be rearranged. Returns true when the list changed.
	bool SoundfontListWidget(const char* id, std::vector<std::string>& fonts, int& selection, bool single)
	{
		bool changed = false;

		const float listHeight = 92.0f;
		ImGui::BeginChild(id, ImVec2(ImGui::GetContentRegionAvail().x - 96.0f, listHeight), ImGuiChildFlags_Borders);

		if (fonts.empty())
			ImGui::TextDisabled("(none picked yet)");

		for (int i = 0; i < (int)fonts.size(); i++)
		{
			ImGui::PushID(i);

			std::filesystem::path path(fonts[i]);
			const std::string name = path.filename().string();

			if (ImGui::Selectable(name.empty() ? fonts[i].c_str() : name.c_str(), selection == i))
				selection = i;

			ImGui::SetItemTooltip("%s", fonts[i].c_str());
			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::BeginGroup();

		if (ImGui::Button("Add...", ImVec2(88, 0)))
		{
			std::string chosen;
			if (Utils::ChooseFile(chosen, "sf2,sf3,sfz,sfark") && !chosen.empty())
			{
				if (single && !fonts.empty())
				{
					// Merlin takes one font; a second choice replaces the first rather than piling up.
					fonts[0] = chosen;
					selection = 0;
				}
				else
				{
					fonts.push_back(chosen);
					selection = (int)fonts.size() - 1;
				}

				changed = true;
			}
		}

		ImGui::BeginDisabled(fonts.empty());
		if (ImGui::Button("Remove", ImVec2(88, 0)))
		{
			if (selection >= 0 && selection < (int)fonts.size())
			{
				fonts.erase(fonts.begin() + selection);
				selection = std::min(selection, (int)fonts.size() - 1);
				changed = true;
			}
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(selection <= 0 || selection >= (int)fonts.size());
		if (ImGui::Button("Up", ImVec2(88, 0)))
		{
			std::swap(fonts[selection], fonts[selection - 1]);
			selection--;
			changed = true;
		}
		ImGui::EndDisabled();

		ImGui::BeginDisabled(selection < 0 || selection >= (int)fonts.size() - 1);
		if (ImGui::Button("Down", ImVec2(88, 0)))
		{
			std::swap(fonts[selection], fonts[selection + 1]);
			selection++;
			changed = true;
		}
		ImGui::EndDisabled();

		ImGui::EndGroup();

		ImGui::Spacing();
		ImGui::TextDisabled("Order matters: each soundfont overrides the ones above it.");

		if (single && fonts.size() > 1)
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Merlin renders one soundfont; only the first is used.");

		return changed;
	}

	void DrawGpuBackendCombo(int& value)
	{
		value = std::clamp(value, 0, 4);

		ImGui::SetNextItemWidth(-1.0f);
		ImGui::Combo("##gpuBackend", &value, GPU_BACKENDS, 5);
	}

	void DrawBlockSizeCombo(int& value)
	{
		// The labels are fixed, so the list is built once instead of per frame.
		static const char* const labels[] = { "512 (11 ms)", "1024 (21 ms)", "2048 (42 ms)",
			"4096 (85 ms)", "8192 (170 ms)" };

		int index = 3;
		for (int i = 0; i < 5; i++)
		{
			if (BLOCK_SIZES[i] == value) index = i;
		}

		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::Combo("##blockSize", &index, labels, 5))
			value = BLOCK_SIZES[index];
	}
}

void KestrelEngine::DrawKestrelPanel()
{
	ImGui::TextDisabled("Kestrel (GPU, Experimental) settings");
	ImGui::Separator();
	ImGui::Spacing();

	DrawSoundfontList();

	ImGui::Spacing();

	ImGui::SetNextItemWidth(160.0f);
	ImGui::InputInt("Max voices", &settings.maxVoices);
	settings.maxVoices = std::clamp(settings.maxVoices, 1, 16777216);

	ImGui::SameLine();
	if (ImGui::Button("Default"))
		settings.maxVoices = 1000000;

	ImGui::Checkbox("Lower it by itself when it can't keep up", &settings.lowerVoicesWhenNeeded);
	ImGui::SetItemTooltip("A render that runs out of video memory is retried with half the voices.");

	ImGui::BeginDisabled(true);
	ImGui::Checkbox("Slow down to keep up", &settings.slowDownToKeepUp);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Kestrel only renders files; it cannot slow playback down, so there is nothing to set.");

	ImGui::Spacing();

	ImGui::Text("Block size:");
	ImGui::SameLine(150.0f);
	DrawBlockSizeCombo(settings.blockSize);

	ImGui::Text("Limiter:");
	ImGui::SameLine(150.0f);
	ImGui::BeginDisabled(true);
	ImGui::SetNextItemWidth(-1.0f);
	int limiter = std::clamp(settings.limiter, 0, 2);
	ImGui::Combo("##limiter", &limiter, LIMITERS, 3);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Kestrel applies its own limiter under the same name; it has no flag to change it.");

	ImGui::Text("Ceiling:");
	ImGui::SameLine(150.0f);
	ImGui::BeginDisabled(true);
	ImGui::SetNextItemWidth(80.0f);
	ImGui::InputInt("##ceiling", &settings.ceilingDb);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Text("dB");

	ImGui::BeginDisabled(true);
	ImGui::Checkbox("DC blocker", &settings.dcBlocker);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Always on inside kestrel.");

	ImGui::Text("Graphics card:");
	ImGui::SameLine(150.0f);
	DrawGpuBackendCombo(settings.gpuBackend);

	ImGui::Text("Interpolation:");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(-1.0f);
	settings.interpolation = std::clamp(settings.interpolation, 0, 2);
	ImGui::Combo("##interp", &settings.interpolation, INTERPOLATIONS, 3);

	ImGui::Text("Velocity threshold:");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(80.0f);
	ImGui::InputInt("##velocityThreshold", &settings.velocityThreshold);
	settings.velocityThreshold = std::clamp(settings.velocityThreshold, 0, 127);
	ImGui::SetItemTooltip("kestrel's --min-velocity: skip every note quieter than this.");
}

void KestrelEngine::DrawRenderedAheadPanel()
{
	ImGui::TextDisabled("Rendered ahead settings");
	ImGui::Separator();
	ImGui::Spacing();

	DrawSoundfontList();

	ImGui::Spacing();

	ImGui::Text("Engine:");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(-1.0f);

	const char* const engines[] = { "Merlin (graphics card)", "Kestrel (graphics card)" };
	settings.engineForAhead = std::clamp(settings.engineForAhead, 0, 1);
	ImGui::Combo("##aheadEngine", &settings.engineForAhead, engines, 2);

	ImGui::BeginDisabled(true);
	ImGui::Selectable("BASSMIDI (processor)", false);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("BASSMIDI plays in Comet through the Prerendered Audio engine; pick that one "
		"for realtime playback without a render.");

	ImGui::BeginDisabled(true);
	ImGui::Checkbox("Use Kestrel where Merlin isn't exact", &settings.useKestrelWhereMerlinInexact);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Kestrel renders one whole song at a time, so it cannot cover the parts of a "
		"Merlin render it is not exact on. Merlin is used on its own.");

	ImGui::Text("Render ahead:");
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(80.0f);
	ImGui::InputInt("##aheadSeconds", &settings.renderAheadSeconds);
	settings.renderAheadSeconds = std::clamp(settings.renderAheadSeconds, 1, 600);
	ImGui::SameLine();
	ImGui::Text("seconds");

	ImGui::BeginDisabled(true);
	ImGui::Checkbox("Slow down to keep up", &settings.slowDownToKeepUp);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Comet's playback speed belongs to the player, not to the renderer.");

	ImGui::Checkbox("Make ahead while stopped", &settings.makeAheadWhileStopped);
	ImGui::SetItemTooltip("Start the render as soon as a song is loaded instead of on the play button.");

	ImGui::Spacing();

	ImGui::Text("Graphics card:");
	ImGui::SameLine(150.0f);
	DrawGpuBackendCombo(settings.gpuBackend);

	ImGui::Text("Block size:");
	ImGui::SameLine(150.0f);
	DrawBlockSizeCombo(settings.blockSize);

	ImGui::Text("Limiter:");
	ImGui::SameLine(150.0f);
	ImGui::BeginDisabled(true);
	ImGui::SetNextItemWidth(-1.0f);
	int aheadLimiter = std::clamp(settings.limiter, 0, 2);
	ImGui::Combo("##aheadLimiter", &aheadLimiter, LIMITERS, 3);
	ImGui::EndDisabled();

	ImGui::Text("Ceiling:");
	ImGui::SameLine(150.0f);
	ImGui::BeginDisabled(true);
	ImGui::SetNextItemWidth(80.0f);
	ImGui::InputInt("##aheadCeiling", &settings.ceilingDb);
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::Text("dB");

	ImGui::BeginDisabled(true);
	ImGui::Checkbox("DC blocker", &settings.dcBlocker);
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::TextDisabled(
		"The first %d seconds are rendered on their own, so playback starts as soon as that chunk "
		"exists; the whole song renders behind it and takes over when the chunk runs out. kestrel "
		"writes the audio as it goes, so nothing waits for the whole file. Merlin can only mix at the "
		"end, so it stays silent until then.",
		settings.renderAheadSeconds
	);
}

void KestrelEngine::DrawSoundfontList()
{
	ImGui::Text("Soundfonts");
	ImGui::Spacing();

	const bool wasSingle = UsesMerlin();
	if (SoundfontListWidget("##soundfonts", settings.soundfonts, soundfontSelection, wasSingle))
	{
		// A changed font list means the cached WAV no longer matches the song's sound.
		CloseChannel();
		cachedWavPath.clear();
	}
}

void KestrelEngine::RenderSettings()
{
	if (mode == Mode::RenderedAhead)
		DrawRenderedAheadPanel();
	else
		DrawKestrelPanel();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::SetNextItemWidth(-120.0f);
	char kestrelBuffer[512];
	std::snprintf(kestrelBuffer, sizeof(kestrelBuffer), "%s", kestrelPath.c_str());
	if (ImGui::InputText("kestrel.exe", kestrelBuffer, sizeof(kestrelBuffer)))
		kestrelPath = kestrelBuffer;

	ImGui::SameLine();
	if (ImGui::Button("Browse##kestrelPath"))
	{
		std::string chosen;
		if (Utils::ChooseFile(chosen, "exe")) kestrelPath = chosen;
	}
	ImGui::SetItemTooltip("Looked for beside Comet as kestrel/kestrel.exe, then kestrel.exe, then on PATH.");

	ImGui::Spacing();

	const std::string currentStatus = GetStatus();
	if (!currentStatus.empty())
		ImGui::TextWrapped("%s", currentStatus.c_str());

	ImGui::BeginDisabled(renderActive.load() || sourcePath.empty());
	if (ImGui::Button("Render now"))
	{
		cachedWavPath.clear();
		std::string wavPath, error;
		EnsureRendered(wavPath, error);
		if (!error.empty()) SetStatus(error);
	}
	ImGui::EndDisabled();

	ImGui::SameLine();

	ImGui::BeginDisabled(!renderActive.load());
	if (ImGui::Button("Cancel render"))
	{
		renderCancel = true;
		SetStatus("Cancelling the render...");
	}
	ImGui::EndDisabled();

	if (sourcePath.empty())
		ImGui::TextDisabled("Load a MIDI to render it.");
}

#endif
