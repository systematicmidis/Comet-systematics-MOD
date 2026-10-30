#pragma once

#include "../Config/MIDIPlayerConfig.h"
#include "../Render/NoteCounter/CounterChartScan.h"
#include "VideoRender/RenderSettings.h"

#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <cstdint>
#include <type_traits>

class MainWindow;
class ThemesList;
class FontList;
class AbstractMIDIRenderer;
class AbstractOverlayRenderer;
class NavigationBar;
class NoteCounterInfo;
class NoteCounterRenderer;
class TrackNoteCounter;
class BlurredQuadRenderer;
class ResourcePackList;
class ColorPaletteList;
class RenderView;
class Progress;
class MIDITimer;
class MIDIAudio;
class Framebuffer;
class Quad;
class FFmpegPipe;
class AbstractMIDILoader;
class MIDISequence;

// Matches Dear ImGui's `enum ImGuiKey : int { ... };` declaration exactly
// (name, underlying type, non-class enum) so we can use it here without
// pulling in the rest of imgui.h.
enum ImGuiKey : int;

class MIDIApp
{
public:
	MIDIApp(MainWindow* mainWindow);
	~MIDIApp();

	AbstractMIDIRenderer* GetRenderer()
	{
		return renderer.get(); // unique_ptr<T>::get() is fine with incomplete T
	}

	template <typename T>
	void SetRenderer();

	bool IsGLReady() const;
	void LoadResources();
	void LoadColorPalettes();
	void LoadMIDI(const char* path);
	void UnloadMIDI();
	void UpdatePendingSequence();
	void RenderMIDIVideo(const RenderSettings& renderSettings);
	void RegisterKeyPress(ImGuiKey key, bool ctrl, bool shift, bool alt);

	// Pushes everything a crash report should know about the current session (crash-report
	// settings, a snapshot of the config, loaded MIDI details) into the crash reporter. Call it
	// whenever any of that changes rather than per frame.
	void UpdateCrashContext();

	// Collects the runtime-owned settings (audio, the active renderer's options) into the config and
	// writes config.yml. Called when the user applies settings and again on exit, so nothing is lost
	// if the app is killed rather than closed.
	void SaveSettings();

	MIDISequence* GetRendererSequence();

	// The note counter and graph count this chart instead of the rendered MIDI when one is loaded.
	// Returns null when the counter simply follows the MIDI.
	const CJCMCGChart* GetCounterChart() const { return counterChart.get(); }
	const std::string& GetCounterSourcePath() const { return counterSourcePath; }
	// Display name of the MIDI loaded last (empty when none).
	const std::string& GetLoadedMIDIPath() const { return pendingPath; }
	// Loads a .CJCMCG chart as the counter/graph source. Returns false and logs on failure.
	bool LoadCounterSource(const char* path);
	void ClearCounterSource();
	MIDIAudio* GetMIDIAudio() { return midiAudio.get(); }
	ThemesList* GetThemeList() { return themesList.get(); }
	FontList* GetFontList() { return fontList.get(); }
	NoteCounterInfo* GetNoteCounterInfo() { return noteCounterInfo.get(); }

	// Per-track plain split for the overlay rates, all measured in one place. The note counter and
	// the Graph overlay both read these, so their numbers come from a single computation and cannot
	// disagree - including the first moment after a MIDI loads or the playhead is seeked.
	uint64_t GetPlainPlayedNotes() const { return plainPlayedNotes; }
	uint64_t GetPlainNotesPerSecond() const { return plainNotesPerSecond; }
	uint64_t GetPlainPolyphony() const { return plainPolyphony; }
	MIDIPlayerConfig* GetConfig() { return &config; }
	MainWindow* GetMainWindow() { return mainWindow; }
	RenderView* GetRenderView() { return renderView.get(); }
	std::shared_ptr<MIDITimer> GetTimer() { return timer; }
	std::shared_ptr<Progress> GetProgress() { return prog; }
	ResourcePackList* GetPackList() { return packList.get(); }
	ColorPaletteList* GetColorList() { return colorList.get(); }
	NavigationBar* GetNavigationBar() { return navigationBar.get(); }

	const RenderSettings& GetCurrentRenderSettings() const { return currentRenderSettings; }
	
	const double GetFPS() const
	{
		return fps;
	}
	const double GetAudioBufferSeconds() const
	{
		return audioBuffer;
	}

	bool IsLoading() const { return loading.load(); }
	bool IsRendering() const { return rendering.load(); }

	void BuildFontAtlas();

	void Update();
	void UpdateNoteCounterInfo();
	void RunFrame();
	void CaptureFrame();
	void OnResize(int width, int height);

	// Body lives in MIDIApp.cpp: constructs MIDILoader/MultithreadedMIDILoader
	// via make_shared, which needs their complete types.
	std::shared_ptr<AbstractMIDILoader> CreateLoader(const char* path);

	bool hasSequence = false;
	std::atomic_bool rendering = false;
	double seqLength = 0.0;
private:
	MainWindow* mainWindow;
	MIDIPlayerConfig config;

	std::unique_ptr<ThemesList> themesList;
	std::unique_ptr<FontList> fontList;
	std::unique_ptr<AbstractMIDIRenderer> renderer;
	std::vector<std::unique_ptr<AbstractOverlayRenderer>> overlays;
	std::unique_ptr<NavigationBar> navigationBar;
	std::shared_ptr<NoteCounterInfo> noteCounterInfo;
	// Owns the per-track cursors behind GetPlain*(); updated once per frame by UpdateOverlayRates().
	std::unique_ptr<TrackNoteCounter> overlayTrackCounter;
	uint64_t plainPlayedNotes = 0;
	uint64_t plainNotesPerSecond = 0;
	uint64_t plainPolyphony = 0;

	// Optional .CJCMCG chart the counter/graph read instead of the rendered MIDI.
	std::shared_ptr<CJCMCGChart> counterChart;
	std::string counterSourcePath = "";
	CounterChartScan counterScan;

	// Fills the GetPlain*() values from the loaded sequence and the current playhead. Called once
	// per frame before the overlays draw, so every overlay sees the same split.
	void UpdateOverlayRates();
	std::unique_ptr<BlurredQuadRenderer> blurredQuadRenderer; // for everything including note counter background, etc.
	std::unique_ptr<ResourcePackList> packList;
	std::shared_ptr<ColorPaletteList> colorList;
	std::shared_ptr<RenderView> renderView;
	std::shared_ptr<Progress> prog;
	std::shared_ptr<MIDITimer> timer;
	std::unique_ptr<MIDIAudio> midiAudio;
	std::atomic_bool loading = false;
	
#pragma region Stats
	double fps = 0.0;
	double audioBuffer = -1.0;
#pragma endregion

#pragma region Framebuffer for rendering
	std::unique_ptr<Framebuffer> renderFramebuffer;
	std::unique_ptr<Quad> fullscreenQuad;
#pragma endregion

	std::mutex appMutex;
	std::mutex thisMtx;
	std::mutex renderMtx;

	double lastRenderStartTimeMs = 0;
	double lastSavedTimeSecs = 0;
	double lastFrameTime = 0;
	RenderSettings currentRenderSettings;
	int currentFrame = 0;
	std::vector<uint8_t> exportPixels{};
	std::unique_ptr<FFmpegPipe> ffmpegPipe;

	// staging variables for thread-safe loading
	std::shared_ptr<MIDISequence> pendingSeq = nullptr;
	std::string pendingTitle = "";
	// Full path of the MIDI currently being loaded, kept for crash reports.
	std::string pendingPath = "";
	std::atomic_bool hasPendingSeq{ false };

	// Cheap per-frame half of UpdateCrashContext: just the flags that change as playback runs.
	void UpdateCrashRuntimeFlags();

	// prepares the app for rendering (disabling navigation, ui, etc.)
	void PrepareRendering();
	// finalizes rendering (re-enables navigation, etc.)
	void FinalizeRendering();
};