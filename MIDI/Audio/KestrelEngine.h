#pragma once

#include "MIDIAudio.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "RenderAhead.h"

#ifdef _WIN32

#include <bass.h>
#include <windows.h>

// Plays a song through Kestrel (https://github.com/thtstickyboi/kestrel-midi), which synthesises on
// the GPU. Kestrel has no realtime output - it renders a file - so this engine renders the song
// with kestrel.exe, then plays the WAV it produced and keeps it in step with the playback timer.
//
// Three entries share the implementation, matching the MIDI-out list:
//   Kestrel (GPU, Experimental) - the GPU renderer, any number of layered soundfonts.
//   Merlin (GPU)                - one soundfont; renders every key for the whole song and mixes.
//   Rendered ahead              - either renderer, started before it is needed.
class KestrelEngine : public AudioEngine
{
public:
	enum class Mode { Gpu, Merlin, RenderedAhead };

	explicit KestrelEngine(Mode mode);
	~KestrelEngine() override;

	std::string GetName() override;
	std::string GetSerializationKey() override;
	bool IsSupported() override { return true; }

	void Initialize() override;
	void Destroy() override;

	void Start(std::shared_ptr<MIDISequence> seq, std::shared_ptr<MIDITimer> timer) override;
	void Stop() override;
	void Reset() override;
	void Mute() override;
	void Unmute() override;
	bool IsPlaying() override;

	YAML::Node GetSettings() override;
	void LoadSettings(const YAML::Node& node) override;
	void RenderSettings() override;

	// kestrel.exe renders the file, not the note data, so the engine needs the path it came from.
	void SetSourcePath(const std::string& path) override;
private:
	// Everything the panels expose. The renderer only takes some of it as flags; the rest is
	// kestrel's own behaviour and is shown for reference.
	struct Settings
	{
		std::vector<std::string> soundfonts;

		int maxVoices = 1000000;
		bool lowerVoicesWhenNeeded = true;
		bool slowDownToKeepUp = false;

		int blockSize = 512;
		int limiter = 1;        // 0 gentle, 1 balanced, 2 loud; kestrel runs its own limiter
		int ceilingDb = 0;      // ditto: kestrel always holds 0 dB
		bool dcBlocker = true;  // ditto

		int gpuBackend = 0;     // 0 automatic, 1 vulkan, 2 dx12, 3 metal, 4 gl
		int interpolation = 1;  // 0 nearest, 1 linear, 2 cubic
		int velocityThreshold = 0;

		int engineForAhead = 0; // 0 Merlin, 1 Kestrel
		bool useKestrelWhereMerlinInexact = false;
		int renderAheadSeconds = 30;
		bool makeAheadWhileStopped = true;
	};

	const Mode mode;
	Settings settings;

	std::string sourcePath;
	std::string kestrelPath;
	int soundfontSelection = 0;

	HSTREAM channel = 0;
	bool bassReady = false;

	std::atomic_bool stopFlag{ false };
	std::atomic_bool playing{ false };
	std::atomic_bool muted{ false };
	std::atomic_bool renderCancel{ false };
	std::atomic_bool renderActive{ false };

	std::thread renderThread;
	std::thread syncThread;

	mutable std::mutex statusMutex;
	std::string status;
	std::string renderLogPath;
	std::string cachedWavPath;

	// Rendered ahead state: kestrel writes "<name>.partial.wav" while it renders and reports
	// "audio_secs" on stdout, so playback follows the bytes that exist.
	std::string partialPath;
	std::atomic<double> renderedAudioSeconds{ 0.0 };
	std::atomic_bool renderFinished{ false };

	std::mutex pcmMutex;
	std::ifstream pcmFile;
	bool pcmReady = false;
	RenderAhead::WavInfo pcmInfo;
	uint64_t pcmBytesPerFrame = 1;
	uint64_t pcmCursorFrames = 0;
	std::atomic<uint64_t> pcmFramesAvailable{ 0 };

	void SetStatus(const std::string& text);
	std::string GetStatus() const;

	bool UsesMerlin() const;
	std::string CachedWavPath() const;
	bool EnsureRendered(std::string& wavPath, std::string& error);
	void RenderLoop();
	void SyncLoop(std::shared_ptr<MIDITimer> timer);
	void OpenChannel(const std::string& wavPath);
	void CloseChannel();

	// --- rendered ahead: play the audio kestrel has already written -------------------------
	bool Streaming() const { return mode == Mode::RenderedAhead; }
	void RenderLoopStreaming();
	bool OpenGrowingWav();
	void RefreshAvailableFrames();
	bool CreatePushStream();
	static DWORD CALLBACK PcmProc(HSTREAM handle, void* buffer, DWORD lengthBytes, void* user);

	void DrawKestrelPanel();
	void DrawRenderedAheadPanel();
	void DrawSoundfontList();
};

#else

class KestrelEngine : public AudioEngine
{
public:
	enum class Mode { Gpu, Merlin, RenderedAhead };
	explicit KestrelEngine(Mode) {}

	void Initialize() override {}
	void Destroy() override {}
	void Start(std::shared_ptr<MIDISequence>, std::shared_ptr<MIDITimer>) override {}
	void Stop() override {}
	void Reset() override {}
	void Mute() override {}
	void Unmute() override {}
	bool IsPlaying() override { return false; }
	void RenderSettings() override {}
	bool IsSupported() override { return false; }
};

#endif
