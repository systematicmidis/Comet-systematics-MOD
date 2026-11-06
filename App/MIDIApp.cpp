#include "MIDIApp.h"
#include "MainWindow.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "../MIDI/AbstractMIDILoader.h"
#include "Render/NoteCounter/AbstractOverlayRenderer.h"
#include <thread>
#include <memory>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "FFmpeg/FFmpegCommandBuilder.h"
#include "Utils.h"
#include "../Render/MIDIRendererPFA.h"
#include "../Render/MIDIRendererSynthesia.h"
#include "../Render/MIDIRendererEnhanced.h"
#include "../Render/MIDIRendererChannels.h"
#include "../Render/MIDIRendererMIDITrail.h"
#include "../Render/MIDIRendererVelocities.h"
#include "../Render/MIDIRendererCounter.h"
#include "../Render/MIDIRenderer.h" // Textured renderer + AbstractMIDIRenderer full definition

#include "../ResourcePack/DefaultResourcePack.h"
#include "../ResourcePack/ResourcePackList.h"
#include "ColorPalette/ColorPaletteList.h"
#include "MIDI/MIDILoader.h"
#include "MIDI/MultithreadedMIDILoader.h"
#include "IO/ArchiveLoader.h"
#include "MIDI/CJCMCGChart.h"
#include "../MIDI/Timer/MIDITimer.h"
#include "App/UI/NavigationBar.h"
#include "../Render/RenderView.h"
#include "Render/NoteCounter/NoteCounterRenderer.h"
#include "Render/NoteCounter/StatsOverlayRenderer.h"
#include "Render/NoteCounter/GraphOverlayRenderer.h"
#include "Render/NoteCounter/TrackNoteCounter.h"
#include "MIDI/MIDIDefs.h"
#include "MIDI/MIDISequence.h"
#include "Render/NoteCounter/NoteCounterInfo.h"
#include "Render/BlurredQuadRenderer.h"
#include "FFmpeg/FFmpegPipe.h"
#include "MIDI/Audio/MIDIAudio.h"
#include "MIDI/Audio/PrerenderEngine/PrerenderedEngine.h"
#include "Models.h"
#include "UI/Themes/Themes.h"
#include "Telemetry/CrashReporter.h"
#include <nlohmann/json.hpp>
#include "FontList.h"

namespace
{
	// A compact view of the settings, so a report says how the app was configured without needing
	// the whole config.yml. The webhook URL is deliberately left out; the reporter strips it too.
	std::string BuildSettingsSnapshot(MIDIPlayerConfig& cfg)
	{
		nlohmann::json settings;

		settings["app"]["themeID"] = cfg.app.currThemeID;

		settings["render"]["renderer"] = ToString(cfg.render.GetCurrentRenderer());
		settings["render"]["width"] = cfg.render.GetWidth();
		settings["render"]["height"] = cfg.render.GetHeight();
		settings["render"]["fpsLimit"] = cfg.render.GetFPSLimit();
		settings["render"]["vsync"] = cfg.render.GetVSync();
		settings["render"]["keyRangeMin"] = cfg.render.GetKeyFirst();
		settings["render"]["keyRangeMax"] = cfg.render.GetKeyLast();
		settings["render"]["showCounter"] = cfg.render.showCounter;
		settings["render"]["showStats"] = cfg.render.showStats;
		settings["render"]["loopColors"] = cfg.render.loopColors;
		settings["render"]["paletteID"] = cfg.render.paletteID;

		settings["overlay"]["style"] = static_cast<int>(cfg.overlayInfo.overlayStyle);
		settings["overlay"]["alignment"] = static_cast<int>(cfg.overlayInfo.overlayAlignment);
		settings["overlay"]["scale"] = cfg.overlayInfo.scale;
		settings["overlay"]["showDuration"] = cfg.overlayInfo.showDuration;
		settings["overlay"]["blurBehind"] = cfg.overlayInfo.blurBehind;

		settings["midi"]["multithreaded"] = cfg.midi.multithreadedLoading;
		settings["midi"]["timeBased"] = cfg.midi.timeBasedLoading;
		settings["midi"]["loadNotesOnly"] = cfg.midi.loadNotesOnly;
		settings["midi"]["cjcmcgPath"] = cfg.midi.cjcmcgPath;

		settings["navigation"]["alwaysHideBar"] = cfg.navigation.alwaysHideBar;
		settings["navigation"]["seekForwardSeconds"] = cfg.navigation.seekForwardSeconds;
		settings["navigation"]["seekBackwardSeconds"] = cfg.navigation.seekBackwardSeconds;

		settings["multiplier"]["enabled"] = cfg.multiplier.enabled;
		settings["multiplier"]["value"] = cfg.multiplier.value;
		settings["multiplier"]["melodyNotes"] = cfg.multiplier.melodyNotes;
		settings["multiplier"]["npsPolyThreshold"] = cfg.multiplier.npsPolyBoostThreshold;
		settings["multiplier"]["randomizePresets"] = cfg.multiplier.randomizePresets;
		settings["multiplier"]["unmultipliedTracks"] = cfg.multiplier.unmultipliedTracks;

		settings["crashReports"]["enabled"] = cfg.crashReports.enabled;

		return settings.dump(2);
	}
}

void MIDIApp::UpdateCrashContext()
{
	CrashReporter::SetEnabled(config.crashReports.enabled);
	// The endpoint is compiled in (CrashReporter::DISCORD_WEBHOOK_URL); nothing here sets it.
	CrashReporter::SetSettingsSnapshot(BuildSettingsSnapshot(config));
}

void MIDIApp::UpdateCrashRuntimeFlags()
{
	CrashReporter::RuntimeFlags flags;
	flags.hasSequence = hasSequence;
	flags.loading = loading.load();
	flags.rendering = rendering.load();
	flags.playing = timer && timer->IsRunning() && !timer->IsPaused();

	CrashReporter::SetRuntimeFlags(flags);
}

MIDIApp::MIDIApp(MainWindow* mainWindow)
{
	config = MIDIPlayerConfig{};
	config.LoadConfigOrDefault();

	UpdateCrashContext();

	renderView = std::make_shared<RenderView>();
	timer = std::make_shared<MIDITimer>();
	navigationBar = std::make_unique<NavigationBar>(this, renderView.get());

	noteCounterInfo = std::make_shared<NoteCounterInfo>();
	overlayTrackCounter = std::make_unique<TrackNoteCounter>();
	// initialize overlays
	overlays.emplace_back(std::make_unique<NoteCounterRenderer>(noteCounterInfo, this));
	overlays.emplace_back(std::make_unique<StatsOverlayRenderer>(this));
	overlays.emplace_back(std::make_unique<GraphOverlayRenderer>(this));
	Models::LoadModels();

	this->mainWindow = mainWindow;

	// Bring back the chart picked last session, if it is still there.
	if (!config.midi.cjcmcgPath.empty() && std::filesystem::exists(config.midi.cjcmcgPath))
		LoadCounterSource(config.midi.cjcmcgPath.c_str());
}

MIDIApp::~MIDIApp()
{
	SaveSettings();
	Models::UnloadModels();
}

void MIDIApp::SaveSettings()
{
	// Audio and the active renderer own their own settings, so fold them back into the config
	// before writing. Doing this on demand (not just at exit) means the settings survive the app
	// being killed instead of closed.
	if (midiAudio)
	{
		config.audioSettings = midiAudio->GetSettings();
	}
	if (renderer)
	{
		config.renderersSettings[renderer->GetSerializationKey()] = renderer->GetSettings();
	}
	config.SaveConfig();
}

bool MIDIApp::IsGLReady() const
{
	if (!mainWindow) return false;
	return mainWindow->IsGLReady();
}

std::shared_ptr<AbstractMIDILoader> MIDIApp::CreateLoader(const char* path)
{
	// Archives are always parsed by the multi-threaded loader: extraction already puts the MIDI in
	// memory, and it is the path that parses mapped/decompressed bytes without copying them again.
	const std::filesystem::path filePath(reinterpret_cast<const char8_t*>(path));
	const bool archive = ArchiveLoader::IsArchive(filePath);

	std::shared_ptr<AbstractMIDILoader> loader;
	if (archive || config.midi.multithreadedLoading)
		loader = std::make_shared<MultithreadedMIDILoader>(path);
	else
		loader = std::make_shared<MIDILoader>(path);

	loader->SetLoadOnlyNotes(config.midi.loadNotesOnly);
	loader->SetLayeredDedupe(config.layeredDedupe.deduplicateRepeatedTracks, config.layeredDedupe.assumeLayerRepeats);
	return loader;
}

void MIDIApp::LoadMIDI(const char* path)
{
	std::lock_guard<std::mutex> lock(appMutex);

	if (loading) return;
	UnloadMIDI();
	loading.store(true);

	{
		std::lock_guard<std::mutex> errorLock(loadErrorMutex);
		hasLoadError = false;
		loadError.clear();
	}
	std::shared_ptr<AbstractMIDILoader> loader = CreateLoader(path);
	prog = loader;

	std::thread([this, loader, pathStr = std::string(path)]() {
		std::shared_ptr<MIDISequence> seq;
		try
		{
			auto* config = this->GetConfig();
			long startMs = static_cast<long>(Utils::GetCurrTime<std::chrono::milliseconds>());
			seq = loader->Load(config->midi.timeBasedLoading);
			long endMs = static_cast<long>(Utils::GetCurrTime<std::chrono::milliseconds>());

			std::cout << "Loaded in " << (endMs - startMs) << "ms" << std::endl;
		}
		catch (const std::bad_alloc&)
		{
			std::cout << "Ran out of memory while loading the MIDI." << std::endl;
			this->SetLoadError("Comet ran out of memory while loading this MIDI. It needs more memory than "
				"this machine has free.");
			this->loading.store(false);
			return;
		}
		catch (const std::runtime_error& e)
		{
			std::cout << "An error occured while loading the MIDI.\n" << e.what() << std::endl;
			this->SetLoadError(e.what());
			this->loading.store(false);
			return;
		}

		bool hasStopped = this->prog->hasStopped.load();

		if (this->prog == loader)
			this->prog = nullptr;

		if (!hasStopped && seq)
		{
			this->pendingTitle = std::filesystem::path(pathStr).filename().string();
			this->pendingPath = pathStr;
			this->pendingSeq = seq;
			this->hasPendingSeq.store(true);
		}
		
		this->loading.store(false);
	}).detach();
	return;
}

bool MIDIApp::LoadCounterSource(const char* path)
{
	if (path == nullptr || *path == '\0')
		return false;

	try
	{
		std::shared_ptr<CJCMCGChart> chart = CJCMCGChart::LoadFile(path);
		if (!chart)
			return false;

		counterChart = chart;
		counterSourcePath = path;
		counterScan = CounterChartScan{};
		config.midi.cjcmcgPath = path;
		SaveSettings();

		std::cout << "CJCMCG counter source loaded: " << path
			<< " (" << chart->tickCount << " ticks, " << chart->totalNotes << " notes)" << std::endl;
		return true;
	}
	catch (const std::exception& e)
	{
		std::cout << "Failed to load CJCMCG file: " << e.what() << std::endl;
		return false;
	}
}

void MIDIApp::ClearCounterSource()
{
	counterChart.reset();
	counterSourcePath.clear();
	counterScan = CounterChartScan{};
	config.midi.cjcmcgPath.clear();
	SaveSettings();
}

void MIDIApp::SetLoadError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(loadErrorMutex);
	loadError = message;
	hasLoadError = true;
}

bool MIDIApp::ConsumeLoadError(std::string& message)
{
	std::lock_guard<std::mutex> lock(loadErrorMutex);
	if (!hasLoadError) return false;

	message = loadError;
	hasLoadError = false;
	loadError.clear();
	return true;
}

void MIDIApp::UnloadMIDI()
{
	if (!hasSequence) return;
	timer->Pause();
	midiAudio->Reset();
	renderer->UnloadSequence();
	hasSequence = false;
	mainWindow->SetTitleInfo();

	CrashReporter::ClearMidiInfo();
	UpdateCrashRuntimeFlags();
}

void MIDIApp::UpdatePendingSequence()
{
	if (!hasPendingSeq.exchange(false)) return;

	std::shared_ptr<MIDISequence> seq = pendingSeq;
	pendingSeq.reset();

	renderer->LoadSequence(seq);
	navigationBar->SetMIDILengthFromSeq(*seq.get());
	noteCounterInfo->ppq.value = seq->resolution;
	hasSequence = true;
	seqLength = seq->CalcLengthMilliseconds() / 1000.0;

	midiAudio->Start(seq, this->timer);
	timer->ClearFlags();
	timer->Start(-3.0);
	mainWindow->SetTitleInfo(pendingTitle);

	// A crash while this file is open is much easier to reproduce with its details on the report.
	CrashReporter::MidiInfo info;
	info.path = pendingPath;
	info.displayName = pendingTitle;
	info.notes = seq->notes;
	info.lengthTicks = seq->length;
	info.trackCount = seq->trackCount;
	info.noteTrackCount = static_cast<uint16_t>(seq->noteTrackCount);
	info.resolution = seq->resolution;
	info.lengthSeconds = seqLength;
	CrashReporter::SetMidiInfo(info);

	UpdateCrashRuntimeFlags();
}

template <typename T>
void MIDIApp::SetRenderer()
{
	static_assert(std::is_base_of_v<AbstractMIDIRenderer, T>, "T must derive from AbstractMIDIRenderer");

	int width = config.render.GetWidth();
	int height = config.render.GetHeight();
	// hacky but oh well
	for (auto& overlay : overlays)
	{
		overlay->OnResize(width, height);
	}

	// save current renderer settings before destroying it
	if (this->renderer)
	{
		std::string oldKey = this->renderer->GetSerializationKey();
		config.renderersSettings[oldKey] = this->renderer->GetSettings();
	}

	// get renderer's sequence so the new one can automatically load it
	std::shared_ptr<MIDISequence> seq;
	if (this->renderer) seq = GetRenderer()->GetSequence();

	this->renderer = std::make_unique<T>(this);
	this->renderer->OnResize(width, height);
	this->renderer->SetNoteCounter(noteCounterInfo);
	this->renderer->Initialize();

	// load new renderer settings IF they exist in config storage
	std::string newKey = this->renderer->GetSerializationKey();
	if (config.renderersSettings[newKey])
	{
		this->renderer->LoadSettings(config.renderersSettings[newKey]);
	}

	if (seq != nullptr) this->renderer->LoadSequence(seq);

	if (blurredQuadRenderer)
		blurredQuadRenderer->SetSceneTexture(this->renderer->GetSceneTexture());

	// ensure the colors are properly loaded if using images for colors
	auto* colorList = GetColorList();
	ColorAsset& colorAsset = this->renderer->GetColorAsset();
	if (config.render.GetUseColorsFromImage())
	{
		colorList->SetPalette(config.render.paletteID);
		auto& palette = colorList->GetCurrentPalette();
		colorAsset.LoadColors(palette.palette, config.render.loopColors);
	}

	// shuffle colors!
	if (config.render.shuffleColors) colorAsset.ShuffleColors(config.render.shuffleSeed);
	else colorAsset.RestoreColors();
}

template void MIDIApp::SetRenderer<MIDIRendererPFA>();
template void MIDIApp::SetRenderer<MIDIRendererSynthesia>();
template void MIDIApp::SetRenderer<MIDIRenderer>();
template void MIDIApp::SetRenderer<MIDIRendererEnhanced>();
template void MIDIApp::SetRenderer<MIDIRendererMIDITrail>();
template void MIDIApp::SetRenderer<MIDIRendererChannels>();
template void MIDIApp::SetRenderer<MIDIRendererVelocities>();
template void MIDIApp::SetRenderer<MIDIRendererCounter>(); // <--- errors here

// called after glfw/glad initialization has finished, and is safe to load stuff, such as images, for rendering
void MIDIApp::LoadResources()
{
	themesList = std::make_unique<ThemesList>("./assets/themes");
	themesList->SetThemeAndApply(config.app.currThemeID);

	auto defaultPack = DefaultResourcePack::Instance();
	defaultPack->Init();

	// load color palettes
	LoadColorPalettes();

	// load resource packs
	packList = std::make_unique<ResourcePackList>();
	packList->RefreshList();

	int width = config.render.GetWidth();
	int height = config.render.GetHeight();

#ifdef COMET_DEBUG
	std::cout << std::endl << "[MIDIApp] Initializing render engine...\n" << std::endl;
#endif
	RendererType currRenderer = config.render.GetCurrentRenderer();

	switch (currRenderer)
	{
	case RendererType::PFA:
		SetRenderer<MIDIRendererPFA>();
		break;
	case RendererType::Synthesia:
		SetRenderer<MIDIRendererSynthesia>();
		break;
	case RendererType::Textured:
		SetRenderer<MIDIRenderer>();
		break;
	case RendererType::Enhanced:
		SetRenderer<MIDIRendererEnhanced>();
		break;
	case RendererType::MIDITrail:
		SetRenderer<MIDIRendererMIDITrail>();
		break;
	case RendererType::Channels:
		SetRenderer<MIDIRendererChannels>();
		break;
	case RendererType::Velocities:
		SetRenderer<MIDIRendererVelocities>();
		break;
	case RendererType::CounterOnly:
		SetRenderer<MIDIRendererCounter>();
		break;
	default:
		SetRenderer<MIDIRendererPFA>();
		break;
	}

	blurredQuadRenderer = std::make_unique<BlurredQuadRenderer>();
	blurredQuadRenderer->SetSceneTexture(renderer->GetSceneTexture());

	renderFramebuffer = std::make_unique<Framebuffer>();
	renderFramebuffer->Setup(width, height);
	fullscreenQuad = std::make_unique<Quad>();
	fullscreenQuad->SetShader(SCENE_SHADER);
	fullscreenQuad->SetTransform({ glm::vec3(0.0f), glm::vec2(1.0f) }, false);
#ifdef COMET_DEBUG
	std::cout << std::endl << "[MIDIApp] Render engine initialized\n" << std::endl;
#endif

	// load audio stuff
	midiAudio = std::make_unique<MIDIAudio>();
	midiAudio->LoadSettings(config.audioSettings);
	midiAudio->GetCurrentEngine()->Initialize();
}

void MIDIApp::BuildFontAtlas()
{
	fontList = std::make_unique<FontList>();

	auto& io = ImGui::GetIO();
	io.Fonts->AddFontDefault();

	fontList->RegisterFontsIntoAtlas(io.Fonts, 18.0f);
}

void MIDIApp::LoadColorPalettes()
{
	if (colorList == nullptr)
		colorList = std::make_shared<ColorPaletteList>();
}

MIDISequence* MIDIApp::GetRendererSequence()
{
	AbstractMIDIRenderer* renderer = GetRenderer();
	if (!renderer) return nullptr;
	return renderer->GetSequence().get();
}

void MIDIApp::Update()
{
	UpdatePendingSequence();

	ImVec4 bgColor = config.render.GetBackground();
	renderFramebuffer->Bind();

	int width = renderFramebuffer->GetWidth();
	int height = renderFramebuffer->GetHeight();

	glViewport(0, 0, width, height);

	float bgAlpha = (rendering && currentRenderSettings.renderTransparencyMask) ? 0.0f : 1.0f;
	glClearColor(bgColor.x, bgColor.y, bgColor.z, bgAlpha);
	glClear(GL_COLOR_BUFFER_BIT);

	if (rendering)
	{
		renderer->Render(1.0 / (double)currentRenderSettings.fps);
	}
	else
	{
		double time = Utils::GetCurrTime<std::chrono::microseconds>() / 1000000.0;
		renderer->Render(time - lastFrameTime);
		UpdateNoteCounterInfo();
		lastFrameTime = time;
	}

	// make sure the render framebuffer is bound
	renderFramebuffer->Bind();
	{
		{
			ShaderBind blurBind(*BLUR_SHADER);
			BLUR_SHADER->SetFloat("width", width);
			BLUR_SHADER->SetFloat("height", height);
			BLUR_SHADER->SetInt("scene", 0);
		}

		{
			ShaderBind sceneBind(*SCENE_SHADER);
			SCENE_SHADER->SetInt("scene", 0);
		}

		TextureBind sceneTextureBind(renderer->GetSceneTexture(), 0);
		fullscreenQuad->Draw();
	}

	UpdateOverlayRates();

	for (auto& overlay : overlays)
	{
		if (!overlay->IsShown()) continue;

		glm::vec2 counterResolution = overlay->GetOverlaySize();
		float heightOffset = rendering || !mainWindow->CanShowNavigationBar() ? 0.0 : navigationBar->GetPosition().y + navigationBar->GetResolution().y;
		glm::vec2 counterPos = overlay->GetOverlayPosition();

		if (config.overlayInfo.blurBehind)
			blurredQuadRenderer->Render({ glm::vec3(counterPos.x, counterPos.y, 0.0f), glm::vec2(counterResolution.x, counterResolution.y) });

		overlay->Render(heightOffset);
	}

	renderFramebuffer->Unbind();

	// render what was in the framebuffer
	{
		int winW, winH;
		GLFWwindow* window = mainWindow->GetInternalWindow();
		glfwGetFramebufferSize(window, &winW, &winH);
		glViewport(0, 0, winW, winH);

		{
			ShaderBind sceneBind(*SCENE_SHADER);
			SCENE_SHADER->SetInt("scene", 0);
		}

		TextureBind sceneTextureBind(renderFramebuffer->GetSceneTexture(), 0);
		fullscreenQuad->Draw();
	}

	if (!rendering)
	{
		if (mainWindow->CanShowNavigationBar())
		{
			// frosted glass effect thingy
			auto size = navigationBar->GetResolution();
			auto pos = navigationBar->GetPosition();

			blurredQuadRenderer->Render({
				glm::vec3(
					pos.x / width,
					1.0f - (pos.y + size.y) / height,
					0.0f
				),
				glm::vec2(
					size.x / width,
					size.y / height
				)
				});

			navigationBar->Draw();
		}

		if (hasSequence && timer->Elapsed() >= seqLength + 3.0 && !timer->IsPaused())
		{
			timer->Pause();
		}
	}
}

void MIDIApp::UpdateOverlayRates()
{
	const MIDISequence* sequence = GetRendererSequence();
	const double playbackSeconds = noteCounterInfo->timeSeconds.value;

	// A loaded chart is the counter/graph source: the MIDI keeps rendering and playing, but the
	// counts come from the chart's timeline. Its ticks are already time-based, so the playhead maps
	// straight to its units. The per-track plain split is about the MIDI's tracks, which the chart
	// doesn't carry, so nothing is treated as plain.
	if (counterChart)
	{
		counterScan.Update(counterChart.get(), playbackSeconds);

		noteCounterInfo->notesPassed.value = counterScan.NotesPassed();
		noteCounterInfo->polyphony.value = counterScan.Polyphony();
		noteCounterInfo->notesPerSecond.value = counterScan.NotesPerSecond();

		plainPlayedNotes = 0;
		plainNotesPerSecond = 0;
		plainPolyphony = 0;
		return;
	}

	// Time-based sequences match the playhead in seconds, the same way the renderers and the note
	// counter do; tick-based ones use the counter's tick.
	const double noteTick = (sequence != nullptr && sequence->timeBased)
		? playbackSeconds * (double)TIME_BASED_MULTIPLIER
		: (double)noteCounterInfo->tick.value;

	overlayTrackCounter->Update(
		sequence,
		playbackSeconds,
		noteTick,
		config.multiplier.unmultipliedTracks,
		true);

	plainPlayedNotes = overlayTrackCounter->PlayedPlain();
	plainNotesPerSecond = overlayTrackCounter->PlainNotesPerSecond();
	plainPolyphony = overlayTrackCounter->PolyphonyPlain();
}

void MIDIApp::UpdateNoteCounterInfo()
{
	double time = Utils::GetCurrTime<std::chrono::microseconds>() / 1000000.0;
	fps = 1.0 / (time - lastFrameTime);
	audioBuffer = -1.0;
	auto* prAudio = dynamic_cast<PrerenderedEngine*>(GetMIDIAudio()->GetCurrentEngine());
	if (prAudio)
	{
		audioBuffer = prAudio->GetBufferSeconds();
	}
}

void MIDIApp::RegisterInput(InputState& inputState)
{
	if (inputState.keyDown)
	{
		MIDIPlayerConfig* config = GetConfig();

		switch (inputState.key)
		{
		case ImGuiKey_Space:
		{
			if (IsRendering()) return;
			// TODO: ignore when no sequence is loaded
			timer->TogglePause();
			break;
		}
		case ImGuiKey_LeftArrow:
		{
			if (IsRendering()) return;
			double currTime = timer->Elapsed();
			double seekTime = std::max(-3.0, currTime - config->navigation.seekBackwardSeconds);
			timer->NavigateTo(seekTime);
			break;
		}
		case ImGuiKey_RightArrow:
		{
			if (IsRendering()) return;
			double currTime = timer->Elapsed();
			double seekTime = std::min(seqLength + 5.0, currTime + config->navigation.seekForwardSeconds);
			timer->NavigateTo(seekTime);
			break;
		}
		// seeks to previous tick
		case ImGuiKey_Comma: // <
		{
			if (IsRendering()) return;
			// VERY hacky, lmfao
			MIDISequence* seq = nullptr;
			if (this->renderer) seq = GetRenderer()->GetSequence().get();
			if (!seq) return;

			TempoMap* tempoMap = seq->GetTempoMap();
			if (!tempoMap) return;

			double currTime = timer->Elapsed();
			long currTick = tempoMap->SecsToTicksFromMap(seq->resolution, currTime);
			timer->NavigateTo(tempoMap->TicksToSecsFromMap(seq->resolution, currTick - 1));
			break;
		}
		case ImGuiKey_Period: // >
		{
			if (IsRendering()) return;

			MIDISequence* seq = nullptr;
			if (this->renderer) seq = GetRenderer()->GetSequence().get();
			if (!seq) return;

			TempoMap* tempoMap = seq->GetTempoMap();
			if (!tempoMap) return;

			double currTime = timer->Elapsed();
			long currTick = tempoMap->SecsToTicksFromMap(seq->resolution, currTime);
			timer->NavigateTo(tempoMap->TicksToSecsFromMap(seq->resolution, currTick + 1));
			break;
		}
		case ImGuiKey_Enter:
		{
			if (!inputState.alt) return;
			mainWindow->ToggleFullscreen();
			break;
		}
		default:
			break;
		}
	}
	
	GetRenderer()->OnInput(inputState);
}

void MIDIApp::OnResize(int width, int height)
{
	if (rendering) return;

	config.render.SetWidth(width);
	config.render.SetHeight(height);

	if (!renderer)
	{
		std::cout << "Renderer uninitialized!" << std::endl;
		return;
	}
	renderer->OnResize(width, height);
	for (auto& overlay : overlays)
	{
		overlay->OnResize(width, height);
	}
	renderFramebuffer->Resize(width, height);
}

void MIDIApp::RunFrame()
{
	if (rendering)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			rendering.store(false);
		}

		double effectiveStart = currentRenderSettings.renderRange
			? currentRenderSettings.rangeStart
			: -currentRenderSettings.midiStartDelay;

		double midiTime = effectiveStart + (double)currentFrame / (double)currentRenderSettings.fps;
		this->timer->NavigateTo(midiTime);

		Update();

		double stopTime = currentRenderSettings.renderRange
			? std::min(seqLength + 5.0, currentRenderSettings.rangeEnd)
			: seqLength + 5.0;

		if (!this->rendering || midiTime >= stopTime)
		{
			std::cout << "Rendering complete or stopped!" << std::endl;
			ffmpegPipe->Close();
			this->FinalizeRendering();
			this->timer->NavigateTo(lastSavedTimeSecs);
			return;
		}
	}
	else
	{
		Update();
	}

	UpdateCrashRuntimeFlags();
}

void MIDIApp::CaptureFrame()
{
	if (!this->rendering) return;

	int width = renderFramebuffer->GetWidth(),
		height = renderFramebuffer->GetHeight();

	renderFramebuffer->Bind();
	glViewport(0, 0, width, height);
	if (currentFrame >= 0)
	{
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		glReadPixels(0, 0, width, height,
			GL_RGBA, GL_UNSIGNED_BYTE, exportPixels.data());
		ffmpegPipe->Write(exportPixels.data(), exportPixels.size());
	}
	renderFramebuffer->Unbind();

	// draw back onto the screen
	{
		int winW, winH;
		GLFWwindow* window = mainWindow->GetInternalWindow();
		glfwGetFramebufferSize(window, &winW, &winH);
		glViewport(0, 0, winW, winH);

		ShaderBind sceneBind(*SCENE_SHADER);
		SCENE_SHADER->SetInt("scene", 0);
		TextureBind sceneTextureBind(renderFramebuffer->GetSceneTexture(), 0);
		fullscreenQuad->Draw();
	}

	currentFrame++;
	if (currentFrame % 10 == 0) std::cout << "Rendered frame " << currentFrame << std::endl;
}

// Will render to a video until cancelled
void MIDIApp::RenderMIDIVideo(const RenderSettings& renderSettings)
{
	this->lastSavedTimeSecs = this->timer->Elapsed();
	this->currentRenderSettings = renderSettings;

	this->PrepareRendering();
	this->currentFrame = -1; // -1 to account for renderers that might need an extra frame to fully settle

	this->exportPixels.resize(renderSettings.width * renderSettings.height * 4);

	// setup FFmpeg
	std::string cmd = FFmpegCommandBuilder::BuildFFmpegCommand(renderSettings);
	std::cout << "Running FFmpeg command: " << cmd << std::endl;
	ffmpegPipe = std::make_unique<FFmpegPipe>();
	if (!ffmpegPipe->Open(cmd))
	{
		std::cout << "Failed to open FFmpeg." << std::endl;
		this->FinalizeRendering();
		return;
	}
}

void MIDIApp::PrepareRendering()
{
	std::lock_guard<std::mutex> lock(renderMtx);
	this->rendering = true;

	int previewWidth = 0;
	int previewHeight = 0;
	GLFWwindow* window = mainWindow->GetInternalWindow();
	glfwGetWindowSize(window, &previewWidth, &previewHeight);

	renderer->OnResize(currentRenderSettings.width, currentRenderSettings.height);
	for (auto& overlay : overlays)
	{
		overlay->OnResize(currentRenderSettings.width, currentRenderSettings.height);
	}
	renderFramebuffer->Resize(currentRenderSettings.width, currentRenderSettings.height);

	lastRenderStartTimeMs = (double)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	if (!timer->IsPaused()) timer->Pause();
	midiAudio->Mute();
	GetRenderer()->ResetRenderer();
}

void MIDIApp::FinalizeRendering()
{
	std::lock_guard<std::mutex> lock(renderMtx);
	this->rendering = false;

	int winW, winH;
	GLFWwindow* window = mainWindow->GetInternalWindow();
	glfwGetFramebufferSize(window, &winW, &winH);
	config.render.SetWidth(winW);
	config.render.SetHeight(winH);

	renderer->OnResize(winW, winH);
	for (auto& overlay : overlays)
	{
		overlay->OnResize(winW, winH);
	}
	renderFramebuffer->Resize(winW, winH);

	double endRenderTime = (double)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	std::cout << "Rendered MIDI in " << Utils::FormatDuration2(endRenderTime - lastRenderStartTimeMs) << "!" << std::endl;

	midiAudio->Unmute();
	GetRenderer()->ResetRenderer();
}