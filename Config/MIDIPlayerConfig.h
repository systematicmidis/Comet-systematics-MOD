#pragma once

#include "imgui.h"

#include "ConfigSection.h"
#include "../MIDI/MIDIDefs.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class RendererType
{
	PFA,
	Synthesia,
	Textured,
	Enhanced,
	MIDITrail,
	Channels,
	Velocities,
	CounterOnly
};

enum class NoteCounterStyle
{
	UMP,
	MIDITrail
};

enum class NoteCounterAlignment
{
	TopLeft,
	TopCenter, // only available for MIDITrail-style counter
	TopRight
};

static const NoteCounterStyle DEFAULT_NOTE_COUNTER_STYLE = NoteCounterStyle::UMP;
static const NoteCounterAlignment DEFAULT_NOTE_COUNTER_ALIGNMENT = NoteCounterAlignment::TopRight;
static const int DEFAULT_NOTE_COUNTER_WIDTH = 200;

inline const std::string ToString(RendererType type)
{
	switch (type)
	{
		case RendererType::PFA:         return "Piano From Above";
		case RendererType::Synthesia:   return "Synthesia";
		case RendererType::Textured:    return "Textured";
		case RendererType::Enhanced:    return "Enhanced Graphics";
		case RendererType::MIDITrail:   return "MIDITrail";
		case RendererType::Channels:    return "Channels";
		case RendererType::Velocities:  return "Velocities";
		case RendererType::CounterOnly: return "Counter Only";
	}

	return "Unknown";
}

struct MIDIPlayerConfig
{
	struct ConfigApp
	{
		size_t currThemeID = 0;
	};

	struct ConfigOverlayInfo
	{
		static constexpr float MIN_SCALE = 0.25f;
		static constexpr float MAX_SCALE = 8.0f;

		float scale = 1.0f;
		std::string selectedFontPath = "";
		bool showDuration = false;
		bool blurBehind = true;

		ImVec4 backgroundCol = ImVec4(0.f, 0.f, 0.f, 0.6f);
		ImVec4 textCol = ImVec4(1.f, 1.f, 1.f, 1.f);

		NoteCounterStyle overlayStyle = DEFAULT_NOTE_COUNTER_STYLE;
		NoteCounterAlignment overlayAlignment = DEFAULT_NOTE_COUNTER_ALIGNMENT;
	};

	struct ConfigOverlay
	{
		bool opaque;
	};

	// The Graph overlay: a scrolling line graph of the counter's NPS and polyphony, drawn on the
	// left by default. Each series has its own colour and the axis labels use the same font as the
	// note counter (overlayInfo.selectedFontPath).
	struct ConfigGraph
	{
		bool enabled = false;

		// Series colours. NPS is red, polyphony cyan, matching the reference look.
		ImVec4 npsColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
		ImVec4 polyColor = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);

		// Draw NPS and polyphony as two stacked graphs (NPS on top, polyphony below), each with its
		// own y-axis, instead of overlaying both lines on one shared axis.
		bool separate = false;

		// Seconds of history kept on screen at once.
		float windowSeconds = 10.0f;
		// Panel size as a fraction of the viewport.
		float widthFraction = 0.32f;
		float heightFraction = 0.40f;

		// How long the y-axis spends zooming back out when a peak scrolls off the left edge. The
		// actual duration slides between these two bounds with the height of the graph: a low graph
		// retracts near the minimum, a tall one near the maximum.
		float zoomOutMinSeconds = 1.0f;
		float zoomOutMaxSeconds = 3.0f;

		NoteCounterAlignment alignment = NoteCounterAlignment::TopLeft;
	};

	struct ConfigMIDI
	{
		bool multithreadedLoading = false;
		bool timeBasedLoading = false;
		bool loadNotesOnly = false;
		bool usePlayThread = true;
		// Path to an optional .CJCMCG chart used as the note counter/graph source. Empty means the
		// counter simply counts the loaded MIDI.
		std::string cjcmcgPath = "";
	};

	struct ConfigNavigation
	{
		bool alwaysHideBar = false;
		float seekForwardSeconds = 10.0f;
		float seekBackwardSeconds = 10.0f;
	};

	// Display-only "note farming" multiplier. It never touches the real note data, the
	// renderer or the audio; it only inflates the counts shown by the note counter overlay.
	//   displayed = (notes - melody) x multiplier + melody
	struct ConfigMultiplier
	{
		bool enabled = false;
		// Decimal digits kept as a string so absurdly large values (googol, centillion...)
		// survive round-tripping through config.yml. "1" means disabled.
		std::string value = "1";
		// Leading notes that are left untouched (the melody).
		uint64_t melodyNotes = 0;
		// NPS/polyphony are only boosted once they climb above this value.
		// 0 boosts unconditionally, whatever the note count.
		uint64_t npsPolyBoostThreshold = 0;
		// Note tracks whose notes are left alone (0-based note-track index, i.e. the track numbers
		// the note data itself carries). Unchecking the melody track here is how you farm only the
		// rest of the song. Empty means every track is multiplied, which is the original behaviour.
		std::vector<uint16_t> unmultipliedTracks{};
		// Preset clicks hand back a random number of the preset's size instead of a round power of
		// ten, so the counter reads like a real note count.
		bool randomizePresets = false;
		// Whether the Graph overlay's NPS/polyphony lines are inflated by the multiplier too, so the
		// graph matches the counter. Off leaves the graph showing the real rates.
		bool multiplyGraph = true;

		bool IsActive() const { return enabled && value != "1"; }

		bool IsTrackMultiplied(uint16_t track) const
		{
			return std::find(unmultipliedTracks.begin(), unmultipliedTracks.end(), track) ==
				unmultipliedTracks.end();
		}

		// True when the multiplier is doing anything and at least one track opted out of it.
		bool SplitsTracks() const { return IsActive() && !unmultipliedTracks.empty(); }

		void SetTrackMultiplied(uint16_t track, bool multiplied)
		{
			const auto it = std::find(unmultipliedTracks.begin(), unmultipliedTracks.end(), track);

			if (multiplied)
			{
				if (it != unmultipliedTracks.end())
					unmultipliedTracks.erase(it);

				return;
			}

			if (it == unmultipliedTracks.end())
			{
				unmultipliedTracks.push_back(track);
				std::sort(unmultipliedTracks.begin(), unmultipliedTracks.end());
			}
		}
	};

	// Crash reporting. The destination is compiled into the app (CrashReporter::DISCORD_WEBHOOK_URL),
	// so there is no URL here for a user or a config file to change; only the on/off switch lives in
	// config. Nothing is captured or sent until the user opts in.
	struct ConfigCrashReports
	{
		// Off by default: nothing is captured or sent until the user opts in.
		bool enabled = false;
	};

	struct ConfigRender
	{
		int GetWidth() { return width; }
		void SetWidth(int width)
		{
			this->width = width;
			if (this->width > 16384)
				this->width = 16384;
			else if (this->width < 128)
				this->width = 128;
		}
		int GetHeight() { return height; }
		void SetHeight(int height)
		{
			this->height = height;
			if (this->height > 16384)
				this->height = 16384;
			else if (this->height < 200)
				this->height = 200;
		}
		int GetFPSLimit()
		{
			return fpsLimit;
		}
		void SetFPSLimit(int fpsLimit)
		{
			this->fpsLimit = fpsLimit;
			if (this->fpsLimit > 240)
				this->fpsLimit = 240;
			else if (this->fpsLimit < 15 && this->fpsLimit > 0)
				this->fpsLimit = 15;
			else if (this->fpsLimit <= 0)
				this->fpsLimit = 0; // uncapped fps xd
		}
		bool GetVSync()
		{
			return vsync;
		}
		void SetVSync(bool vsync)
		{
			this->vsync = vsync;
		}
		ImVec4 GetBackground()
		{
			return background;
		}
		void SetBackground(float r, float g, float b)
		{
			background.x = r;
			background.y = g;
			background.z = b;
		}

		ImVec4 GetBarColor()
		{
			return barColor;
		}
		void SetBarColor(float r, float g, float b)
		{
			barColor.x = r;
			barColor.y = g;
			barColor.z = b;
		}

		bool GetUseColorsFromImage() { return useColorsFromImage; }
		void SetUseColorsFromImage(bool useColors) { useColorsFromImage = useColors; }

		RendererType GetCurrentRenderer() { return currentRenderer; }
		void SetCurrentRenderer(RendererType renderer)
		{
			currentRenderer = renderer;
		}

		int GetKeyFirst() { return keyFirst; }
		void SetKeyFirst(int key)
		{
			if (key < 0) key = 0;
			if (key > keyLast) key = keyLast;
			
			if (keyFirst != key)
			{
				keyFirst = key;
				keyRangeChanged = true;
			}
		}

		int GetKeyLast() { return keyLast; }
		void SetKeyLast(int key)
		{
			if (key > MIDI_KEYS - 1) key = MIDI_KEYS - 1;
			if (key < keyFirst) key = keyFirst;
			
			if (keyLast != key)
			{
				keyLast = key;
				keyRangeChanged = true;
			}
		}

		bool ConsumeKeyRangeChanged()
		{
			bool tmp = keyRangeChanged;
			keyRangeChanged = false;
			return tmp;
		}

		bool showCounter = true;
		bool showStats = true;
		bool loopColors = true;
		int paletteID = 0;
	private:
		int width = 1280;
		int height = 720;
		int fpsLimit = 120;
		bool vsync = true;
		double renderInterval = 5.0;
		ImVec4 background = ImVec4(0.0, 0.0, 0.0, 1.0);
		ImVec4 barColor = ImVec4(0.52, 0.0, 0.0, 1.0);
		RendererType currentRenderer = RendererType::PFA;
		
		bool usePFAColors = false;
		bool useColorsFromImage = false;
		
		int keyFirst = 0;
		int keyLast = 127;
		bool keyRangeChanged = false;

		std::string resourcePack = "";
		std::string font = "Monospaced";
		MIDIPlayerConfig::ConfigOverlay overlay{};
	};

	int version = 0;
	std::string language = "en";

	ConfigApp app{};
	ConfigMIDI midi{};
	ConfigNavigation navigation{};
	ConfigRender render{};
	ConfigOverlayInfo overlayInfo{};
	ConfigGraph graph{};
	ConfigMultiplier multiplier{};
	ConfigCrashReports crashReports{};

	YAML::Node audioSettings;
	YAML::Node renderersSettings;

	void LoadConfigOrDefault();
	void SaveConfig();
};