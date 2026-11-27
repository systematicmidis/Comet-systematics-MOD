#include "MIDIPlayerConfig.h"
#include <algorithm>
#include <yaml-cpp/yaml.h>
#include "ConfigSection.h"
#include <optional>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include "Utils.h"
#include <filesystem>

#define LOAD_VAL(n, key, target) \
        if (n && n[key]) { target = n[key].as<std::decay_t<decltype(target)>>(); }

#define LOAD_COLOR(n, key, var, def) \
		uint32_t col = Utils::PackRGBA(def.x, def.y, def.z, def.w, Utils::ARGB); \
		LOAD_VAL(n, key, col); \
		auto var = Utils::UnpackRGBA(col, Utils::ARGB);

void MIDIPlayerConfig::LoadConfigOrDefault()
{
	if (std::getenv("COMET_SAFE_MODE") != nullptr)
	{
		std::cout << "Safe mode: ignoring config.yml and using defaults" << std::endl;
		return;
	}

	std::optional<ConfigSection> config;
	try
	{
		std::ifstream stream("./config.yml", std::ios::in);
		if (!stream.is_open())
		{
			std::cout << "Failed to load config.yml. Using default config" << std::endl;
			return;
		}

		YAML::Node root = YAML::Load(stream);
		config = ConfigSection(root);

		const int configVersion = config->GetInt("version", 1);
		if (configVersion < 3)
			std::cout << "Migrating config version " << configVersion
				<< " to 3; removed features keep their defaults." << std::endl;

		std::optional<ConfigSection> appSec = config->GetSection("app");
		
		if (appSec)
		{
			ConfigApp app;
			app.currThemeID = appSec->GetInt("themeID", 0);
			this->app = app;
		}

		std::optional<ConfigSection> overlaySec = config->GetSection("overlay");
		if (overlaySec)
		{
			ConfigOverlayInfo overlay;
			overlay.scale = overlaySec->GetFloat("scale", 1.0f);
			overlay.blurBehind = overlaySec->GetBoolean("blurBehind", true);
			overlay.selectedFontPath = overlaySec->GetString("fontPath", "");

			{
				LOAD_COLOR(root["overlay"], "background", bgColRgba, overlay.backgroundCol);
				overlay.backgroundCol = ImVec4(bgColRgba[0], bgColRgba[1], bgColRgba[2], bgColRgba[3]);
			}

			{
				LOAD_COLOR(root["overlay"], "text", txtColRgba, overlay.textCol);
				overlay.textCol = ImVec4(txtColRgba[0], txtColRgba[1], txtColRgba[2], 1.0);
			}

			overlay.overlayStyle = static_cast<NoteCounterStyle>(overlaySec->GetInt("style", static_cast<int>(DEFAULT_NOTE_COUNTER_STYLE)));
			overlay.overlayAlignment = static_cast<NoteCounterAlignment>(overlaySec->GetInt("alignment", static_cast<int>(DEFAULT_NOTE_COUNTER_ALIGNMENT)));

			this->overlayInfo = overlay;
		}

		std::optional<ConfigSection> graphSec = config->GetSection("graph");
		if (graphSec)
		{
			ConfigGraph graph;
			graph.enabled = graphSec->GetBoolean("enabled", false);
			graph.separate = graphSec->GetBoolean("separate", false);
			graph.windowSeconds = graphSec->GetFloat("windowSeconds", 10.0f);
			graph.widthFraction = graphSec->GetFloat("widthFraction", 0.32f);
			graph.heightFraction = graphSec->GetFloat("heightFraction", 0.40f);
			graph.zoomOutMinSeconds = graphSec->GetFloat("zoomOutMinSeconds", 1.0f);
			graph.zoomOutMaxSeconds = graphSec->GetFloat("zoomOutMaxSeconds", 3.0f);
			graph.alignment = static_cast<NoteCounterAlignment>(graphSec->GetInt("alignment", static_cast<int>(NoteCounterAlignment::TopLeft)));

			{
				LOAD_COLOR(root["graph"], "npsColor", npsRgba, graph.npsColor);
				graph.npsColor = ImVec4(npsRgba[0], npsRgba[1], npsRgba[2], 1.0f);
			}

			{
				LOAD_COLOR(root["graph"], "polyColor", polyRgba, graph.polyColor);
				graph.polyColor = ImVec4(polyRgba[0], polyRgba[1], polyRgba[2], 1.0f);
			}

			this->graph = graph;
		}

		std::optional<ConfigSection> bannerSec = config->GetSection("banner");
		if (bannerSec)
		{
			ConfigBanner banner;
			banner.enabled = bannerSec->GetBoolean("enabled", true);
			banner.showInPreview = bannerSec->GetBoolean("showInPreview", false);
			banner.synthesiaScale = std::clamp(bannerSec->GetFloat("synthesiaScale", 1.0f),
				MIN_SYNTHESIA_BANNER_SCALE, MAX_SYNTHESIA_BANNER_SCALE);
			banner.umpScale = std::clamp(bannerSec->GetFloat("umpScale", 1.0f),
				MIN_UMP_BANNER_SCALE, MAX_UMP_BANNER_SCALE);
			banner.animation = static_cast<BannerAnimation>(std::clamp(
				bannerSec->GetInt("animation", (int)BannerAnimation::OpenFromMiddle), 0, 3));
			banner.animationSeconds = std::clamp(bannerSec->GetFloat("animationSeconds", 0.55f),
				0.05f, 3.0f);
			banner.imagePath = bannerSec->GetString("imagePath", "");
			banner.imageAlignment = static_cast<BannerImageAlignment>(std::clamp(
				bannerSec->GetInt("imageAlignment", (int)BannerImageAlignment::Left), 0, 2));
			banner.imageScale = std::clamp(bannerSec->GetFloat("imageScale", 0.75f), 0.1f, 1.0f);
			banner.imageOffsetX = std::clamp(bannerSec->GetFloat("imageOffsetX", 0.0f), -0.5f, 0.5f);
			banner.imageOffsetY = std::clamp(bannerSec->GetFloat("imageOffsetY", 0.0f), -0.5f, 0.5f);
			{
				LOAD_COLOR(root["banner"], "umpBackground", umpBgRgba, banner.umpBackgroundCol);
				banner.umpBackgroundCol = ImVec4(umpBgRgba[0], umpBgRgba[1], umpBgRgba[2], umpBgRgba[3]);
			}
			{
				LOAD_COLOR(root["banner"], "umpTitleColor", umpTitleRgba, banner.umpTitleCol);
				banner.umpTitleCol = ImVec4(umpTitleRgba[0], umpTitleRgba[1], umpTitleRgba[2], 1.0f);
			}
			{
				LOAD_COLOR(root["banner"], "umpCreditColor", umpCreditRgba, banner.umpCreditCol);
				banner.umpCreditCol = ImVec4(umpCreditRgba[0], umpCreditRgba[1], umpCreditRgba[2], 1.0f);
			}
			this->banner = banner;
		}

		std::optional<ConfigSection> midiSec = config->GetSection("midi");
		if (midiSec)
		{
			ConfigMIDI midi;
			midi.multithreadedLoading = midiSec->GetBoolean("multithreaded", true);
			midi.timeBasedLoading = midiSec->GetBoolean("timeBased", false);
			midi.cjcmcgPath = midiSec->GetString("cjcmcgPath", "");

			bool deduplicateLayers = midiSec->GetBoolean("deduplicateLayers", true);
			if (!(root["midi"] && root["midi"]["deduplicateLayers"]))
			{
				// Configs written before the sub-options were merged keep their old main switch.
				std::optional<ConfigSection> legacyDedupe = config->GetSection("layeredDeDupe");
				if (legacyDedupe)
					deduplicateLayers = legacyDedupe->GetBoolean("deduplicateRepeatedTracks", true);
			}
			midi.deduplicateLayers = deduplicateLayers;

			this->midi = midi;
		}

		rememberFilePresets = config->GetBoolean("rememberFilePresets", false);
		if (root["filePresets"] && root["filePresets"].IsMap())
		{
			for (const auto& entry : root["filePresets"])
			{
				const std::string key = entry.first.as<std::string>("");
				if (!key.empty() && entry.second.IsMap())
					filePresets[key] = entry.second;
			}
		}

		std::optional<ConfigSection> navigationSec = config->GetSection("navigation");
		if (navigationSec)
		{
			ConfigNavigation navigation;
			navigation.alwaysHideBar = navigationSec->GetBoolean("alwaysHideBar", false);
			navigation.seekForwardSeconds = navigationSec->GetFloat("seekForwardSeconds", 10.0f);
			navigation.seekBackwardSeconds = navigationSec->GetFloat("seekBackwardSeconds", 10.0f);

			this->navigation = navigation;
		}

		std::optional<ConfigSection> multiplierSec = config->GetSection("multiplier");
		if (multiplierSec)
		{
			ConfigMultiplier multiplier;
			multiplier.enabled = multiplierSec->GetBoolean("enabled", false);
			multiplier.value = Utils::NormalizeDecimalDigits(multiplierSec->GetRawString("value", "1"));
			multiplier.melodyNotes = (uint64_t)std::max(0, multiplierSec->GetInt("melodyNotes", 0));
			multiplier.npsPolyBoostThreshold = (uint64_t)std::max(0, multiplierSec->GetInt("npsPolyThreshold", 0));
			multiplier.randomizePresets = multiplierSec->GetBoolean("randomizePresets", false);
			multiplier.multiplyGraph = multiplierSec->GetBoolean("multiplyGraph", true);

			// Tracks the multiplier leaves alone. Stored as a sequence so it can grow with the file.
			const YAML::Node unmultiplied = root["multiplier"] && root["multiplier"]["unmultipliedTracks"]
				? root["multiplier"]["unmultipliedTracks"]
				: YAML::Node();

			if (unmultiplied && unmultiplied.IsSequence())
			{
				for (const YAML::Node& entry : unmultiplied)
				{
					// Every note packs at most 13 bits of track index, so anything larger is junk.
					const int track = entry.as<int>(-1);
					if (track >= 0 && track < 8192)
						multiplier.unmultipliedTracks.push_back((uint16_t)track);
				}
			}

			this->multiplier = multiplier;
		}

		std::optional<ConfigSection> crashSec = config->GetSection("crashReports");
		if (crashSec)
		{
			ConfigCrashReports crashReports;
			crashReports.enabled = crashSec->GetBoolean("enabled", false);
			this->crashReports = crashReports;
		}

		std::optional<ConfigSection> renderSec = config->GetSection("render");
		if (renderSec)
		{
			ConfigRender render;
			render.SetWidth(renderSec->GetInt("width", 1280));
			render.SetHeight(renderSec->GetInt("height", 720));

			render.SetFPSLimit(renderSec->GetInt("fpsLimit", 120));
			render.SetVSync(renderSec->GetBoolean("vsync", true));

			{
				LOAD_COLOR(root["render"], "background", bgColRgba, render.GetBackground());
				render.SetBackground(bgColRgba[0], bgColRgba[1], bgColRgba[2]);
			}

			{
				LOAD_COLOR(root["render"], "bar", barColRgba, render.GetBarColor());
				render.SetBarColor(barColRgba[0], barColRgba[1], barColRgba[2]);
			}

			render.SetUseColorsFromImage(renderSec->GetBoolean("useImageColors", false));
			render.loopColors = renderSec->GetBoolean("loopColors", true);
			render.shuffleColors = renderSec->GetBoolean("shuffleColors", false);
			render.shuffleSeed = renderSec->GetInt("shuffleSeed", 0);
			render.paletteID = renderSec->GetInt("paletteID", 0);

			render.SetCurrentRenderer(static_cast<RendererType>(renderSec->GetInt("rendererType", 0)));

			std::optional<ConfigSection> keyRangeSec = renderSec->GetSection("keyRange");
			if (keyRangeSec)
			{
				render.SetKeyFirst(keyRangeSec->GetInt("min", 0));
				render.SetKeyLast(keyRangeSec->GetInt("max", 127));
			}

			render.showCounter = renderSec->GetBoolean("showCounter", true);
			render.showStats = renderSec->GetBoolean("showStats", true);
			render.showScore = renderSec->GetBoolean("showScore", false);
			render.scoreScale = std::clamp(renderSec->GetInt("scoreScale", 50), 15, 100);

			this->render = render;
		}

		if (root["audio"])
		{
			audioSettings = root["audio"];

			// Before version 3, index 0 was OmniMIDI, 1 was Prerendered and 2 was Maestro.
			// OmniMIDI is gone, so keep a saved internal engine instead of resetting it.
			if (configVersion < 3 && audioSettings["currentEngine"])
			{
				const int legacyEngine = audioSettings["currentEngine"].as<int>(-1);
				audioSettings["currentEngine"] = legacyEngine == 2 ? 1 : 0;
			}
		}
		
		if (root["renderers"])
		{
			renderersSettings = root["renderers"];
		}
	}
	catch (...)
	{
		std::cout << "Failed to load config (config.yml). Backing it up and using defaults." << std::endl;

		// Keep the bad file so the user can inspect it, but stop every launch from failing on the
		// same malformed YAML. A previous backup is retained as config.yml.broken.prev.
		std::error_code error;
		const std::filesystem::path original("./config.yml");
		if (std::filesystem::exists(original, error))
		{
			const std::filesystem::path backup("./config.yml.broken");
			const std::filesystem::path previous("./config.yml.broken.prev");
			std::filesystem::remove(previous, error);
			if (std::filesystem::exists(backup, error))
				std::filesystem::rename(backup, previous, error);
			std::filesystem::rename(original, backup, error);
		}
	}
}

void MIDIPlayerConfig::SaveConfig()
{
	YAML::Node config;
	config["version"] = 3;

	config["app"]["themeID"] = app.currThemeID;

	config["overlay"]["scale"] = overlayInfo.scale;
	config["overlay"]["blurBehind"] = overlayInfo.blurBehind;
	config["overlay"]["fontPath"] = overlayInfo.selectedFontPath;
	config["overlay"]["background"] = Utils::EncodeColor(overlayInfo.backgroundCol);
	config["overlay"]["text"] = Utils::EncodeColor(overlayInfo.textCol);
	config["overlay"]["style"] = static_cast<int>(overlayInfo.overlayStyle);
	config["overlay"]["alignment"] = static_cast<int>(overlayInfo.overlayAlignment);

	config["graph"]["enabled"] = graph.enabled;
	config["graph"]["separate"] = graph.separate;
	config["graph"]["windowSeconds"] = graph.windowSeconds;
	config["graph"]["widthFraction"] = graph.widthFraction;
	config["graph"]["heightFraction"] = graph.heightFraction;
	config["graph"]["zoomOutMinSeconds"] = graph.zoomOutMinSeconds;
	config["graph"]["zoomOutMaxSeconds"] = graph.zoomOutMaxSeconds;
	config["graph"]["alignment"] = static_cast<int>(graph.alignment);
	config["graph"]["npsColor"] = Utils::EncodeColor(graph.npsColor);
	config["graph"]["polyColor"] = Utils::EncodeColor(graph.polyColor);

	config["banner"]["enabled"] = banner.enabled;
	config["banner"]["showInPreview"] = banner.showInPreview;
	config["banner"]["synthesiaScale"] = banner.synthesiaScale;
	config["banner"]["umpBackground"] = Utils::EncodeColor(banner.umpBackgroundCol);
	config["banner"]["umpTitleColor"] = Utils::EncodeColor(banner.umpTitleCol);
	config["banner"]["umpCreditColor"] = Utils::EncodeColor(banner.umpCreditCol);
	config["banner"]["umpScale"] = banner.umpScale;
	config["banner"]["animation"] = static_cast<int>(banner.animation);
	config["banner"]["animationSeconds"] = banner.animationSeconds;
	config["banner"]["imagePath"] = banner.imagePath;
	config["banner"]["imageAlignment"] = static_cast<int>(banner.imageAlignment);
	config["banner"]["imageScale"] = banner.imageScale;
	config["banner"]["imageOffsetX"] = banner.imageOffsetX;
	config["banner"]["imageOffsetY"] = banner.imageOffsetY;

	config["midi"]["multithreaded"] = midi.multithreadedLoading;
	config["midi"]["timeBased"] = midi.timeBasedLoading;
	config["midi"]["deduplicateLayers"] = midi.deduplicateLayers;
	config["midi"]["cjcmcgPath"] = midi.cjcmcgPath;

	config["navigation"]["alwaysHideBar"] = navigation.alwaysHideBar;
	config["navigation"]["seekForwardSeconds"] = navigation.seekForwardSeconds;
	config["navigation"]["seekBackwardSeconds"] = navigation.seekBackwardSeconds;

	config["multiplier"]["enabled"] = multiplier.enabled;
	config["multiplier"]["value"] = multiplier.value;
	config["multiplier"]["melodyNotes"] = (long long)multiplier.melodyNotes;
	config["multiplier"]["npsPolyThreshold"] = (long long)multiplier.npsPolyBoostThreshold;
	config["multiplier"]["randomizePresets"] = multiplier.randomizePresets;
	config["multiplier"]["multiplyGraph"] = multiplier.multiplyGraph;
	config["multiplier"]["unmultipliedTracks"] = YAML::Node(YAML::NodeType::Sequence);
	for (uint16_t track : multiplier.unmultipliedTracks)
		config["multiplier"]["unmultipliedTracks"].push_back((int)track);

	config["crashReports"]["enabled"] = crashReports.enabled;
	config["rememberFilePresets"] = rememberFilePresets;

	{
		YAML::Node presets;
		for (const auto& [path, preset] : filePresets)
		{
			if (!path.empty() && preset.IsMap())
				presets[path] = preset;
		}
		config["filePresets"] = presets;
	}

	config["render"]["width"] = render.GetWidth();
	config["render"]["height"] = render.GetHeight();
	config["render"]["fpsLimit"] = render.GetFPSLimit();
	config["render"]["vsync"] = render.GetVSync();
	config["render"]["background"] = Utils::EncodeColor(render.GetBackground());
	config["render"]["bar"] = Utils::EncodeColor(render.GetBarColor());
	config["render"]["useImageColors"] = render.GetUseColorsFromImage();
	config["render"]["loopColors"] = render.loopColors;
	config["render"]["shuffleColors"] = render.shuffleColors;
	config["render"]["shuffleSeed"] = render.shuffleSeed;
	config["render"]["paletteID"] = render.paletteID;
	config["render"]["rendererType"] = static_cast<int>(render.GetCurrentRenderer());
	config["render"]["keyRange"]["min"] = render.GetKeyFirst();
	config["render"]["keyRange"]["max"] = render.GetKeyLast();

	config["render"]["showCounter"] = render.showCounter;
	config["render"]["showStats"] = render.showStats;
	config["render"]["showScore"] = render.showScore;
	config["render"]["scoreScale"] = render.scoreScale;

	if (audioSettings.IsDefined())
	{
		config["audio"] = audioSettings;
	}

	if (renderersSettings.IsDefined())
	{
		config["renderers"] = renderersSettings;
	}

	// Write through a temporary file and keep the previous good copy. A crash or a full disk
	// can then leave either the old config or the new one, never a half-written YAML file.
	std::error_code error;
	if (std::filesystem::exists("./config.yml", error))
	{
		std::filesystem::copy_file("./config.yml", "./config.yml.bak",
			std::filesystem::copy_options::overwrite_existing, error);
	}

	const std::filesystem::path target("./config.yml");
	const std::filesystem::path temporary("./config.yml.tmp");
	{
		std::ofstream file(temporary);
		file << config;
	}

	std::filesystem::rename(temporary, target, error);
	if (error)
	{
		// Some filesystems do not replace an existing file with rename; remove and retry.
		std::filesystem::remove(target, error);
		std::filesystem::rename(temporary, target, error);
	}
}