#include "SettingsDialog.h"
#include "Utils.h"
#include "App/MainWindow.h"
#include "Render/MIDIRendererPFA.h"
#include "Render/MIDIRendererSynthesia.h"
#include "Render/MIDIRendererEnhanced.h"
#include "Render/MIDIRendererMIDITrail.h"
#include "Render/MIDIRendererChannels.h"
#include "Render/MIDIRendererVelocities.h"
#include "Render/MIDIRendererCounter.h"
#include "Render/NoteCounter/NoteCounterRenderer.h"
#include "DialogMacros.h"
#include "App/UI/Themes/Themes.h"
#include "ColorPalette/ColorPaletteList.h"
#include "ResourcePack/ResourcePackList.h"
#include "App/FontList.h"
#include "MIDI/Audio/MIDIAudio.h"
#include "MIDI/MIDISequence.h"
#include "Render/NoteCounter/NoteMultiplier.h"
#include "Telemetry/CrashReporter.h"
#include "Telemetry/CometLog.h"
#include <cstdio>
#include <thread>

namespace
{
	std::string Pow10(int exponent)
	{
		return "1" + std::string((size_t)std::max(0, exponent), '0');
	}

	// A centillion (10^303) times a whole note count is already 312 digits, so anything past this
	// can only come from a typo or from digits piling up in the box.
	constexpr std::size_t MAX_COUNTER_DIGITS = 320;

	// Digits only, no leading zeros. Empty when the box holds nothing usable.
	std::string ParseCount(const char* text)
	{
		std::string digits;
		for (const char* c = text; c != nullptr && *c != '\0'; ++c)
		{
			if (*c >= '0' && *c <= '9')
				digits.push_back(*c);
		}

		const std::size_t firstNonZero = digits.find_first_not_of('0');
		if (firstNonZero == std::string::npos)
			return std::string();

		return digits.substr(firstNonZero);
	}

	// Short enough to sit under a settings row whatever the multiplier has grown to.
	std::string PrettyDigits(const std::string& digits)
	{
		if (digits.size() <= 7)
			return Utils::FormatWithCommas(digits);

		return Utils::AbbreviateBigNumber(digits);
	}

	// How many digits the multiplier currently has (NormalizeDecimalDigits never returns empty).
	std::size_t SignificantDigits(const std::string& digits)
	{
		return Utils::NormalizeDecimalDigits(digits).size();
	}

	// "Track 7 - Melody", kept short enough for the settings label column.
	std::string TrackLabel(std::size_t track, const std::string& name)
	{
		std::string label = "Track " + std::to_string(track + 1);
		if (name.empty())
			return label;

		std::string shown = name;
		if (shown.size() > 14)
			shown = shown.substr(0, 13) + ".";

		return label + " - " + shown;
	}

	const ImVec4 WARNING_COLOR(1.0f, 0.45f, 0.35f, 1.0f);
}

namespace
{
	template <typename Enum, typename DrawContentFn>
	void DrawSubTabs(const char* id, const char* heading,
		Enum& selected, float sidebarWidth,
		std::initializer_list<std::pair<const char*, Enum>> tabs,
		DrawContentFn&& drawContent)
	{
		ImGui::PushID(id);

		// Left sidebar
		ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, 0.0f),
			ImGuiChildFlags_Borders);

		{
			const ImVec4 headingColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);

			ImGui::TextColored(
				ImVec4(headingColor.x, headingColor.y, headingColor.z, 0.5f), "%s", heading);

			ImGui::Spacing();

			for (const auto& [label, value] : tabs)
			{
				if (ImGui::Selectable(label, selected == value)) selected = value;
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		// Right content panel
		ImGui::BeginChild("Content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		{
			drawContent();
		}
		ImGui::EndChild();

		ImGui::PopID();
	}
}

void SettingsDialog::DrawContent()
{
	const float footerHeight =
		ImGui::GetFrameHeightWithSpacing() + 2.0f * ImGui::GetStyle().ItemSpacing.y;

	if (ImGui::BeginChild("SettingsBody", ImVec2(0.0, -footerHeight), ImGuiChildFlags_None))
	{
		if (ImGui::BeginTabBar("MainTabs"))
		{
			if (ImGui::BeginTabItem("App"))
			{
				DrawAppTab();

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Visual"))
			{
				DrawVisualTab();

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Audio"))
			{
				DrawAudioTab();

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("MIDI"))
			{
				DrawMIDITab();

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Multiplier"))
			{
				DrawMultiplierTab();

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}

	ImGui::EndChild();
	ImGui::Separator();
	if (ImGui::Button("Apply & Close"))
	{
		// "Apply" really does write the settings out: without this the file was only written on a
		// clean exit, so a kill or a crash lost every change made in the dialog.
		app->SaveSettings();
		ImGui::CloseCurrentPopup();
	}
}

void SettingsDialog::DrawAppTab()
{
	auto config = app->GetConfig();
	ThemesList* themesList = app->GetThemeList();

	enum AppSubTab
	{
		General,
		Theme,
		Navigation,
		CrashReports
	};

	static AppSubTab appSubTab = General;

	DrawSubTabs("AppSettings", "APP SETTINGS",
		appSubTab, 200.0f, {
			{ "General", General },
			{ "Theme", Theme },
			{ "Navigation", Navigation },
			{ "Crash Reports", CrashReports }
		},
		[&] {
			switch (appSubTab)
			{
			case General:
			{
				SECTION_HEADER("Rendering");

				BEGIN_SECTION("##renderSec")
				{
					SETUP_SECTION;
					SECTION_ENTRY(SECTION_LABEL("VSync"),
						{
							bool vsync = config->render.GetVSync();
							if (ImGui::Checkbox("##vsync", &vsync))
							{
								config->render.SetVSync(vsync);
								glfwSwapInterval(vsync);
							}
						});

					SECTION_ENTRY({ SECTION_LABEL("FPS Limit"); ImGui::SetItemTooltip("When set to 0 then the FPS will be uncapped"); },
						{
							int fpsLimit = config->render.GetFPSLimit();
							if (ImGui::InputInt("##fpsLimit", &fpsLimit))
							{
								config->render.SetFPSLimit(fpsLimit);
							}
						});
						END_SECTION;
				}

				break;
			}
			case Theme:
			{
				size_t themeIdx = 0;

				if (ImGui::Button("Reload theme list"))
					themesList->ReloadThemesList();

				ImGui::SameLine();

				if (ImGui::Button("Open themes folder"))
					themesList->OpenThemeListFolder();

				ImGui::Separator();

				for (const auto& theme : themesList->GetThemesList())
				{
					AppTheme* themePtr = theme.get();
					bool isSelected = themePtr == themesList->GetCurrentTheme();
					ImGui::PushID(themePtr);

					float width = ImGui::GetContentRegionAvail().x;
					float height = 50.0f;
					ImVec2 pos = ImGui::GetCursorScreenPos();

					if (ImGui::Selectable("##themeBtn", isSelected, 0, ImVec2(width, height)))
					{
						app->GetThemeList()->SetThemeAndApply(themeIdx);

						ThemeInfo& themeInfo = themePtr->info;
						std::cout << "Loaded \"" << themeInfo.name << "\"" << std::endl
							<< "  By " << themeInfo.author << std::endl
							<< "  Desc:" << themeInfo.description << std::endl
							<< "  Version: " << themeInfo.version << std::endl;

						config->app.currThemeID = themeIdx;
					}

					bool isHovered = ImGui::IsItemHovered();
					ImDrawList* drawList = ImGui::GetWindowDrawList();

					// draw card background
					auto toU32 = [](const ImVec4& color) { return ImGui::ColorConvertFloat4ToU32(color); };

					ImVec4 selectColor = ImGui::GetStyleColorVec4(ImGuiCol_TabSelectedOverline);
					selectColor.w = 20;
					ImU32 bgColor = isSelected ? toU32(selectColor) : (isHovered ? IM_COL32(150, 150, 150, 30) : IM_COL32(0, 0, 0, 0));
					drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(isSelected ? ImGuiCol_CheckMark : ImGuiCol_Border), 4.0f, 0, 2.0f);

					// draw swatch previews for controls n stuff
					float swatchSize = 26.0f;
					float padding = 12.0f;
					ImVec2 swatchPos = ImVec2(pos.x + padding, pos.y + (height - swatchSize) * 0.5f);

					ImU32 outlineColor = IM_COL32(50, 50, 50, 100);

					// bg swatch
					drawList->AddRectFilled(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), toU32(theme->colors.background), 4.0f);
					drawList->AddRect(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), outlineColor, 4.0f);
					swatchPos.x += swatchSize + 4;

					// control swatch
					drawList->AddRectFilled(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), toU32(theme->colors.controlBase), 4.0f);
					drawList->AddRect(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), outlineColor, 4.0f);
					swatchPos.x += swatchSize + 4;

					// accent swatch
					drawList->AddRectFilled(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), toU32(theme->colors.accent), 4.0f);
					drawList->AddRect(swatchPos, ImVec2(swatchPos.x + swatchSize, swatchPos.y + swatchSize), outlineColor, 4.0f);
					swatchPos.x += swatchSize + 4;

					// draw theme's name and author
					ImVec2 textPos = ImVec2(swatchPos.x + 15.0f, pos.y + 8.0f);
					drawList->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), theme->info.name.c_str());

					ImVec2 subTextPos = ImVec2(textPos.x, textPos.y + ImGui::GetTextLineHeight() + 2.0f);
					drawList->AddText(subTextPos, ImGui::GetColorU32(ImGuiCol_TextDisabled), ("by " + theme->info.author).c_str());

					ImGui::PopID();
					themeIdx++;
				}

				break;
			}
			case Navigation:
			{
				BEGIN_SECTION("##navi")
				{
					SETUP_SECTION;

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Always hide navigation bar",
							"Enabling this will always hide the menu/navigation bar, unless the mouse is near it."
						),
						ImGui::Checkbox("##hideNavigationbar", &config->navigation.alwaysHideBar);
					);

					SECTION_ENTRY(
						SECTION_LABEL("Jump forward secs"),
						{
							float jumpForwardSecs = config->navigation.seekForwardSeconds;
							if (ImGui::InputFloat("##forwardSecs", &jumpForwardSecs, ImGuiInputTextFlags_NoHorizontalScroll))
							{
								config->navigation.seekForwardSeconds = std::clamp(jumpForwardSecs, 0.0001f, 10.0f);
							}
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Jump backward secs"),
						{
							float jumpBackwardSecs = config->navigation.seekBackwardSeconds;
							if (ImGui::InputFloat("##backwardSecs", &jumpBackwardSecs, ImGuiInputTextFlags_NoHorizontalScroll))
							{
								config->navigation.seekBackwardSeconds = std::clamp(jumpBackwardSecs, 0.0001f, 10.0f);
							}
						}
					);

					END_SECTION;
				}

				break;
			}
			case CrashReports:
			{
				DrawCrashReportsTab();

				break;
			}
			}
		});
}

void SettingsDialog::ApplyCrashConfiguration(bool saveNow)
{
	app->UpdateCrashContext();

	// Crash-report settings are saved immediately: if Comet dies, the usual save on exit never
	// happens, and the helper that uploads the report reads the on/off switch back out of
	// config.yml.
	if (saveNow)
		app->GetConfig()->SaveConfig();
}

void SettingsDialog::DrawCrashReportsTab()
{
	auto config = app->GetConfig();

	SECTION_HEADER("Crash reports");

	ImGui::TextWrapped(
		"When Comet crashes it can send a report so the failure can be looked at without having to "
		"reproduce it. Reporting is off until you turn it on.");

	ImGui::Spacing();

	bool enabled = config->crashReports.enabled;
	if (ImGui::Checkbox("Send a crash report when Comet crashes", &enabled))
	{
		config->crashReports.enabled = enabled;
		ApplyCrashConfiguration(true);
	}

	ImGui::SetItemTooltip(
		"Reports go to the fixed Discord webhook and nowhere else. Nothing is uploaded while this "
		"is off.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// The destination is intentionally not shown or editable here: it is compiled into the app
	// (CrashReporter::DISCORD_WEBHOOK_URL) and can only be changed in the source.
	ImGui::BeginDisabled(crashTest->inFlight.load());
	if (ImGui::Button("Send test report"))
	{
		const std::string webhook = CrashReporter::DISCORD_WEBHOOK_URL;
		const auto state = crashTest;
		state->inFlight.store(true);
		state->succeeded.store(false);

		// Off the UI thread: an unreachable webhook would otherwise freeze the window for the
		// whole connect timeout.
		std::thread([state, webhook]()
		{
			const std::string report = CrashReporter::BuildReport(CrashReporter::KIND_TEST,
				"Test report sent from the Settings dialog.");

			std::string error;
			const bool ok = CrashReporter::SendOrQueue(report, webhook, &error);

			{
				std::lock_guard<std::mutex> lock(state->mutex);
				state->message = ok ? std::string("Sent. Check your Discord channel.") : error;
			}

			state->succeeded.store(ok);
			state->inFlight.store(false);
		}).detach();
	}
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button("Copy report to clipboard"))
	{
		const std::string report = CrashReporter::BuildReport(CrashReporter::KIND_TEST,
			"Copied from the Settings dialog; this report was not sent anywhere.");
		ImGui::SetClipboardText(report.c_str());
	}

	if (crashTest->inFlight.load())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("sending...");
	}
	else
	{
		std::lock_guard<std::mutex> lock(crashTest->mutex);
		if (!crashTest->message.empty())
		{
			if (crashTest->succeeded.load())
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", crashTest->message.c_str());
			else
				ImGui::TextWrapped("Could not send: %s", crashTest->message.c_str());
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	SECTION_HEADER("Queued reports");

	const int pending = CrashReporter::CountPending();
	ImGui::Text("Waiting to be sent: %d", pending);
	ImGui::TextWrapped("Folder: %s", CrashReporter::ReportDirectory().c_str());

	if (ImGui::Button("Open folder"))
		Utils::OpenFolder(CrashReporter::ReportDirectory());

	ImGui::SameLine();
	ImGui::BeginDisabled(pending == 0);
	if (ImGui::Button("Delete queued reports"))
		CrashReporter::DeletePending();
	ImGui::EndDisabled();

	ImGui::TextDisabled(
		"A crash is captured by a fresh helper process, not by Comet itself, so a crash inside the "
		"network stack can still be reported. Anything the helper can't deliver waits here and is "
		"retried on the next launch.");

	ImGui::Spacing();

	if (ImGui::CollapsingHeader("What exactly gets sent?"))
	{
		ImGui::BulletText("Comet version, build type and compiler");
		ImGui::BulletText("Windows version, architecture, CPU, RAM, locale and timezone");
		ImGui::BulletText("GPU vendor, renderer and OpenGL version");
		ImGui::BulletText("Exception code, faulting module and address");
		ImGui::BulletText("A symbolised call stack, with file and line where a .pdb is available");
		ImGui::BulletText("The loaded MIDI's path, note count, track count and length");
		ImGui::BulletText("Playback and render state, plus a snapshot of the settings");
		ImGui::BulletText("The last %d lines Comet printed to the console", (int)CometLog::MAX_LINES);
		ImGui::BulletText("The webhook URL is stripped out before anything is uploaded");

		ImGui::Spacing();
		ImGui::TextWrapped(
			"Use \"Copy report to clipboard\" above to see the exact JSON that would be sent, without "
			"sending it.");
	}
}

void SettingsDialog::DrawVisualTab()
{
	auto config = app->GetConfig();

	ImGui::Text("These settings will also apply when rendering videos!");
	ImGui::Separator();

	enum VisualSubTab
	{
		Renderer,
		NoteColors,
		CounterAndStats
	};

	static VisualSubTab visualSubTab = Renderer;

	DrawSubTabs("VisualSettings", "VISUAL SETTINGS",
		visualSubTab, 160.0f, {
			{ "Renderer", Renderer },
			{ "Note Colors", NoteColors },
			{ "Counter & Stats", CounterAndStats }
		},
		[&] {
			switch (visualSubTab)
			{
			case Renderer:
			{
				MIDIPlayerConfig* config = app->GetConfig();
				RendererType currRenderer = config->render.GetCurrentRenderer();

				BEGIN_SECTION("##renderer")
				{
					SETUP_SECTION;

					#define RENDERER_COMBO_ENTRY(enumTarget, target) \
						if (ImGui::Selectable(ToString(enumTarget).c_str(), currRenderer == enumTarget)) \
						{ \
							if (currRenderer != enumTarget) \
							{ \
								config->render.SetCurrentRenderer(enumTarget); \
								app->SetRenderer<target>(); \
							} \
						}

					SECTION_ENTRY(
						SECTION_LABEL("Renderer"),
						{
							std::string currentRenderer = ToString(currRenderer);
							if (ImGui::BeginCombo("##rendererCombo", currentRenderer.c_str()))
							{

								RENDERER_COMBO_ENTRY(RendererType::PFA, MIDIRendererPFA);
								RENDERER_COMBO_ENTRY(RendererType::Synthesia, MIDIRendererSynthesia);
								RENDERER_COMBO_ENTRY(RendererType::Textured, MIDIRenderer);
								RENDERER_COMBO_ENTRY(RendererType::Enhanced, MIDIRendererEnhanced);
								RENDERER_COMBO_ENTRY(RendererType::MIDITrail, MIDIRendererMIDITrail);
								RENDERER_COMBO_ENTRY(RendererType::Channels, MIDIRendererChannels);
								RENDERER_COMBO_ENTRY(RendererType::Velocities, MIDIRendererVelocities);
								RENDERER_COMBO_ENTRY(RendererType::CounterOnly, MIDIRendererCounter);

								ImGui::EndCombo();
							}
						}
					);

#undef RENDERER_COMBO_ENTRY

					END_SECTION;
				}

				ImGui::Spacing();

				if (ImGui::CollapsingHeader("Renderer Settings", ImGuiTreeNodeFlags_DefaultOpen))
				{
					app->GetRenderer()->RenderSettings();
				}

				break;
			}
			case NoteColors:
			{
				ColorPaletteList* colorList = app->GetColorList();
				auto* config = app->GetConfig();

				if (colorList != nullptr)
				{
					bool setColorsFromPack = false;
					bool setColorsFromList = false;

					BEGIN_SECTION("##noteColors")
					{
						SETUP_SECTION;

						SECTION_ENTRY(
							SECTION_LABEL("Use colors from"),
							if (ImGui::RadioButton("Resource pack", !config->render.GetUseColorsFromImage()))
							{
								config->render.SetUseColorsFromImage(false);
								setColorsFromPack = true;
							}

						if (ImGui::RadioButton("Color list", config->render.GetUseColorsFromImage()))
						{
							config->render.SetUseColorsFromImage(true);
							setColorsFromList = true;
						}
							);

						if (setColorsFromList)
						{
							auto& entry = colorList->GetCurrentPalette();
							app->GetRenderer()->LoadColors(entry.palette);
						}
						else if (setColorsFromPack)
						{
							ResourcePackList* packList = app->GetPackList();
							auto currPack = packList->GetActivePack();
							auto noteColors = currPack->GetStream("noteColors.png");

							auto& colorAsset = app->GetRenderer()->GetColorAsset();
							if (noteColors != nullptr)
							{
								colorAsset.LoadColors(noteColors, currPack->GetNoteInfo()->loopColors);
							}
							else
							{
								colorAsset.ResetColors();
								colorAsset.LoadColors();
							}
						}

						END_SECTION;
					}

					SECTION_HEADER("Palette list");

					ImGui::BeginDisabled(!config->render.GetUseColorsFromImage());
					if (ImGui::Button("Refresh palette list"))
					{
						colorList->ReloadList();
						auto& entry = colorList->GetCurrentPalette();
						app->GetRenderer()->LoadColors(entry.palette);
					}
					ImGui::SameLine();
					if (ImGui::Button("Open palette folder"))
					{
						Utils::OpenFolder("./colors");
					}

					ImGui::Spacing();

					auto& palettes = colorList->GetPalettes();
					const ColorPaletteEntry& currPalette = colorList->GetCurrentPalette();

					ImGui::BeginChild("PaletteScroll", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
					ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));

					for (size_t i = 0; i < palettes.size(); ++i)
					{
						const auto& palette = palettes[i];
						bool isActivePalette = (&palette == &currPalette);

						ImGui::PushID((int)i);
						ImGui::BeginGroup();

						ImVec2 cardSize(ImGui::GetContentRegionAvail().x, 62.0f);
						if (ImGui::Selectable("##palette_card", isActivePalette,
							0, cardSize))
						{
							colorList->SetPalette(i);
							config->render.paletteID = i;
							app->GetRenderer()->GetColorAsset().LoadColors(palette.palette, true);
						}

						ImDrawList* dl = ImGui::GetWindowDrawList();
						ImVec2 min = ImGui::GetItemRectMin();
						ImVec2 max = ImGui::GetItemRectMax();

						ImU32 bg = isActivePalette
							? ImGui::GetColorU32(ImGuiCol_FrameBgActive)
							: ImGui::GetColorU32(ImGuiCol_FrameBg);

						dl->AddRect(min, max, ImGui::GetColorU32(isActivePalette ? ImGuiCol_CheckMark : ImGuiCol_Border), 4.0f, 0, 2.0f);

						// Palette name
						ImVec2 textPos(min.x + 12.0f, min.y + 10.0f);
						dl->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), palette.name.c_str());

						// Small color preview strip
						float swatchY = min.y + 34.0f;
						float swatchW = 26.0f;
						float swatchH = 14.0f;
						float swatchX = min.x + 12.0f;
						const int previewCount = (int)std::min<size_t>(10, palette.palette.size());

						for (int s = 0; s < previewCount; ++s)
						{
							const auto& c = palette.palette[s];
							ImU32 col = IM_COL32(c[0] * 255, c[1] * 255, c[2] * 255, 255);

							ImVec2 a(swatchX + s * (swatchW + 4.0f), swatchY);
							ImVec2 b(a.x + swatchW, a.y + swatchH);
							dl->AddRectFilled(a, b, col, 4.0f);
							dl->AddRect(a, b, IM_COL32(0, 0, 0, 60), 4.0f);
						}

						if (isActivePalette)
						{
							ImVec2 badgePos(max.x - 72.0f, min.y + 10.0f);
							dl->AddText(badgePos, ImGui::GetColorU32(ImGuiCol_CheckMark), "In use");
						}

						ImGui::EndGroup();
						ImGui::PopID();
					}

					ImGui::PopStyleVar();
					ImGui::EndChild();

					BEGIN_SECTION("##noteColorsEnd")
					{
						SETUP_SECTION;

						SECTION_ENTRY(
							SECTION_LABEL("Loop colors"),
							if (ImGui::Checkbox("##loopColors", &config->render.loopColors))
							{
								auto& currPalette = colorList->GetCurrentPalette();
								ColorAsset& colorAsset = app->GetRenderer()->GetColorAsset();
								colorAsset.LoadColors(currPalette.palette, config->render.loopColors);
							}
								);

						SECTION_ENTRY(
							SECTION_LABEL("Shuffle colors"),
							if (ImGui::Checkbox("##shuffleColors", &config->render.shuffleColors))
							{
								ColorAsset& colorAsset = app->GetRenderer()->GetColorAsset();
								if (config->render.shuffleColors)
									colorAsset.ShuffleColors((uint32_t)config->render.shuffleSeed);
								else
									colorAsset.RestoreColors();
							}
								);

						if (config->render.shuffleColors)
						{
							SECTION_ENTRY(
								SECTION_LABEL("Seed"),
								if (ImGui::InputInt("##shuffleSeed", &config->render.shuffleSeed))
								{
									if (config->render.shuffleSeed < 0) config->render.shuffleSeed = 0;
									ColorAsset& colorAsset = app->GetRenderer()->GetColorAsset();
									if (config->render.shuffleColors) colorAsset.ShuffleColors((uint32_t)config->render.shuffleSeed);
								}
									);
						}

						END_SECTION;
					}
					ImGui::EndDisabled();
				}

				break;
			}
			case CounterAndStats:
			{
				MIDIPlayerConfig* config = app->GetConfig();
				NoteCounterInfo* counter = app->GetNoteCounterInfo();
				FontList* fontList = app->GetFontList();
				std::vector<FontEntry>& fonts = fontList->GetFonts();

				const NoteCounterAlignment alignment = config->overlayInfo.overlayAlignment;
				const NoteCounterStyle style = config->overlayInfo.overlayStyle;

				SECTION_HEADER("Appearance");

				BEGIN_SECTION("##nc_appearance")
				{
					SETUP_SECTION;

					SECTION_ENTRY(
						SECTION_LABEL("Show note counter"),
						ImGui::Checkbox("##showNoteCounter", &config->render.showCounter);
					);

					SECTION_ENTRY(
						SECTION_LABEL("Show statistics"),
						ImGui::Checkbox("##showStats", &config->render.showStats);
					);

					SECTION_ENTRY(
						SECTION_LABEL("Scale"),
						{
							ImGui::SliderFloat(
								"##counterScale",
								&config->overlayInfo.scale,
								config->overlayInfo.MIN_SCALE,
								config->overlayInfo.MAX_SCALE
							);
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Alignment"),
						{

							if (ImGui::RadioButton(
								"Top Left",
								alignment == NoteCounterAlignment::TopLeft))
							{
								config->overlayInfo.overlayAlignment = NoteCounterAlignment::TopLeft;
							}

							ImGui::BeginDisabled(
								style != NoteCounterStyle::MIDITrail
							);

							ImGui::SameLine();

							if (ImGui::RadioButton(
								"Top Center",
								alignment == NoteCounterAlignment::TopCenter))
							{
								config->overlayInfo.overlayAlignment = NoteCounterAlignment::TopCenter;
							}

							ImGui::EndDisabled();

							ImGui::SameLine();

							if (ImGui::RadioButton(
								"Top Right",
								alignment == NoteCounterAlignment::TopRight))
							{
								config->overlayInfo.overlayAlignment = NoteCounterAlignment::TopRight;
							}
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Blur behind"),
						ImGui::Checkbox(
							"##blurBehind",
							&config->overlayInfo.blurBehind
						);
					);

					SECTION_ENTRY(SECTION_LABEL("Font"),
						{
							std::string currentFontName = "Default";

							for (const auto& f : fonts)
							{
								if (f.path == config->overlayInfo.selectedFontPath)
								{
									currentFontName = f.name;
									break;
								}
							}

							if (ImGui::BeginCombo(
								"##counterFontCombo",
								currentFontName.c_str()))
							{
								if (ImGui::Selectable(
									"Default",
									config->overlayInfo.selectedFontPath.empty()))
								{
									config->overlayInfo.selectedFontPath.clear();
								}

								for (const auto& f : fonts)
								{
									const bool isSelected =
										config->overlayInfo.selectedFontPath == f.path;

									if (ImGui::Selectable(
										f.name.c_str(),
										isSelected))
									{
										config->overlayInfo.selectedFontPath = f.path;
									}

									if (isSelected)
										ImGui::SetItemDefaultFocus();
								}

								ImGui::EndCombo();
							}
						});

					SECTION_ENTRY(SECTION_LABEL("Font directory"),
						{
							if (ImGui::Button("Open"))
							{
								Utils::OpenFolder("./assets/fonts/");
							}
						});

					END_SECTION;
				}


				SECTION_HEADER("Style");

				BEGIN_SECTION("##nc_style")
				{
					SETUP_SECTION;

					SECTION_ENTRY(
						SECTION_LABEL("Style"),
						{
							if (ImGui::RadioButton(
								"Ultralight MIDI Player",
								style == NoteCounterStyle::UMP))
							{
								config->overlayInfo.overlayStyle = NoteCounterStyle::UMP;

								if (config->overlayInfo.overlayAlignment == NoteCounterAlignment::TopCenter)
								{
									config->overlayInfo.overlayAlignment = NoteCounterAlignment::TopRight;
								}
							}

							ImGui::SameLine();

							if (ImGui::RadioButton(
								"MIDITrail",
								style == NoteCounterStyle::MIDITrail))
							{
								config->overlayInfo.overlayStyle = NoteCounterStyle::MIDITrail;
							}
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Background color"),
						{
							ImVec4 backgroundColor = config->overlayInfo.backgroundCol;
							if (ImGui::ColorEdit4("##ncBg", &backgroundColor.x, ImGuiColorEditFlags_AlphaBar))
							{
								config->overlayInfo.backgroundCol = backgroundColor;
							}
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Text color"),
						{
							ImVec4 textColor = config->overlayInfo.textCol;
							if (ImGui::ColorEdit3("##ncTxtCol", &textColor.x))
							{
								config->overlayInfo.textCol = textColor;
							}
						}
					);

					END_SECTION;
				}

				SECTION_HEADER("Fields");
				ImGui::TextDisabled("Fields marked with an asterisk (*) are omitted from video renders.");
				ImGui::Spacing();

				BEGIN_SECTION("##nc_fields")
				{
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Checkbox("Tick", &counter->tick.shown);
					ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Time", &counter->timeSeconds.shown);

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Checkbox("BPM", &counter->bpm.shown);
					ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Notes", &counter->notesPassed.shown);

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::Checkbox("NPS", &counter->notesPerSecond.shown);
					ImGui::TableSetColumnIndex(1); ImGui::Checkbox("Polyphony", &counter->polyphony.shown);

					END_SECTION;
				}

				SECTION_HEADER("Graph");
				ImGui::TextDisabled(
					"A scrolling line graph of NPS (red) and polyphony (cyan), drawn on the left by default. "
					"Its numbers use the font chosen above.");
				ImGui::Spacing();

				BEGIN_SECTION("##nc_graph")
				{
					SETUP_SECTION;

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Show graph",
							"Draw the NPS/polyphony graph overlay. It follows the note counter's values, so the "
							"multiplier affects it too unless you turn that off in the Multiplier tab."
						),
						ImGui::Checkbox("##showGraph", &config->graph.enabled);
					);

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Separate lines",
							"Draw NPS and polyphony as two stacked graphs - NPS on top, polyphony below - each "
							"scaling to its own data, instead of overlaying both on one shared axis."
						),
						ImGui::Checkbox("##graphSeparate", &config->graph.separate);
					);

					SECTION_ENTRY(
						SECTION_LABEL("NPS color"),
						{
							ImVec4 npsColor = config->graph.npsColor;
							if (ImGui::ColorEdit3("##graphNpsColor", &npsColor.x))
								config->graph.npsColor = npsColor;
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Polyphony color"),
						{
							ImVec4 polyColor = config->graph.polyColor;
							if (ImGui::ColorEdit3("##graphPolyColor", &polyColor.x))
								config->graph.polyColor = polyColor;
						}
					);

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Position",
							"Which side of the screen the graph hugs."
						),
						{
							const NoteCounterAlignment graphAlignment = config->graph.alignment;

							if (ImGui::RadioButton("Left##graphAlign", graphAlignment == NoteCounterAlignment::TopLeft))
								config->graph.alignment = NoteCounterAlignment::TopLeft;

							ImGui::SameLine();

							if (ImGui::RadioButton("Center##graphAlign", graphAlignment == NoteCounterAlignment::TopCenter))
								config->graph.alignment = NoteCounterAlignment::TopCenter;

							ImGui::SameLine();

							if (ImGui::RadioButton("Right##graphAlign", graphAlignment == NoteCounterAlignment::TopRight))
								config->graph.alignment = NoteCounterAlignment::TopRight;
						}
					);

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Seconds shown",
							"How much history stays on screen before it scrolls off."
						),
						{
							ImGui::SetNextItemWidth(-1.0f);
							ImGui::SliderFloat("##graphWindow", &config->graph.windowSeconds, 1.0f, 60.0f, "%.1f s");
						}
					);

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Zoom-out min",
							"How quickly the y-axis comes back down when a peak scrolls off the left edge, for a "
							"short graph. A taller graph takes longer, up to the max below."
						),
						{
							ImGui::SetNextItemWidth(-1.0f);
							if (ImGui::SliderFloat("##graphZoomOutMin", &config->graph.zoomOutMinSeconds, 0.1f, 10.0f, "%.2f s"))
								if (config->graph.zoomOutMaxSeconds < config->graph.zoomOutMinSeconds)
									config->graph.zoomOutMaxSeconds = config->graph.zoomOutMinSeconds;
						}
					);

					SECTION_ENTRY(
						TABLE_LABEL_TOOLTIP(
							"Zoom-out max",
							"How slowly the y-axis comes back down for a very tall graph."
						),
						{
							ImGui::SetNextItemWidth(-1.0f);
							if (ImGui::SliderFloat("##graphZoomOutMax", &config->graph.zoomOutMaxSeconds, 0.1f, 10.0f, "%.2f s"))
								if (config->graph.zoomOutMinSeconds > config->graph.zoomOutMaxSeconds)
									config->graph.zoomOutMinSeconds = config->graph.zoomOutMaxSeconds;
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Width"),
						{
							ImGui::SetNextItemWidth(-1.0f);
							ImGui::SliderFloat("##graphWidth", &config->graph.widthFraction, 0.1f, 1.0f, "%.2f of screen");
						}
					);

					SECTION_ENTRY(
						SECTION_LABEL("Height"),
						{
							ImGui::SetNextItemWidth(-1.0f);
							ImGui::SliderFloat("##graphHeight", &config->graph.heightFraction, 0.1f, 1.0f, "%.2f of screen");
						}
					);

					END_SECTION;
				}

				break;
			}
			}
		});
}

void SettingsDialog::DrawAudioTab()
{
	MIDIAudio* midiAudio = app->GetMIDIAudio();

	// ImGui::Text("Audio Engine");
	BEGIN_SECTION("##audioEngine")
	{
		SETUP_SECTION;

		SECTION_ENTRY(
			SECTION_LABEL("Audio engine"),
			{
				AudioEngineList& engineList = midiAudio->GetEngineList();
				size_t engineIndex = 0;
				for (auto& engine : engineList)
				{
					if (engine == nullptr)
					{
						engineIndex++;
						continue;
					}

					AudioEngineType type = (AudioEngineType)engineIndex;

					bool isSupported = engine->IsSupported();

					ImGui::BeginDisabled(!isSupported);
					if (ImGui::RadioButton(engine->GetName().c_str(), midiAudio->GetCurrentEngineType() == type))
						midiAudio->SwitchEngine(type);

					if (!isSupported)
						ImGui::SetItemTooltip("This engine is not supported on your platform.");
					ImGui::EndDisabled();

					ImGui::Spacing();

					engineIndex++;
				}
			}
		);

		END_SECTION;
	}
	
	if (ImGui::CollapsingHeader("Engine Settings", ImGuiTreeNodeFlags_DefaultOpen))
	{
		midiAudio->GetCurrentEngine()->RenderSettings();
	}
}

void SettingsDialog::DrawMIDITab()
{
	auto* config = app->GetConfig();

	BEGIN_SECTION("##midiSettings")
	{
		SETUP_SECTION;

		SECTION_ENTRY(
			SECTION_LABEL("MIDI loading threads"),
			{
				if (ImGui::RadioButton("Single-threaded", !config->midi.multithreadedLoading))
					config->midi.multithreadedLoading = false;

				if (ImGui::RadioButton("Multi-threaded", config->midi.multithreadedLoading))
					config->midi.multithreadedLoading = true;
			}
		);

		SECTION_ENTRY(
			SECTION_LABEL("Time-based loading"),
			{ ImGui::Checkbox("##timebasedLoading", &config->midi.timeBasedLoading); }
		);

		SECTION_ENTRY(
			TABLE_LABEL_TOOLTIP(
				"MIDI file",
				"The song that is drawn, played and listened to. Open it here or from File > Open."
			),
			{
				const std::string& midiPath = app->GetLoadedMIDIPath();
				ImGui::TextWrapped("%s", midiPath.empty() ? "(none)" : midiPath.c_str());
				if (ImGui::Button("Open..."))
					app->GetMainWindow()->OpenMIDIDialog();
			}
		);

		SECTION_ENTRY(
			TABLE_LABEL_TOOLTIP(
				"CJCMCG note source",
				"An optional .CJCMCG chart. When loaded, the note counter and the graph count this chart's "
				"note timeline instead of the MIDI's notes, while the MIDI keeps being drawn and played."
			),
			{
				const std::string& chartPath = app->GetCounterSourcePath();
				ImGui::TextWrapped("%s", chartPath.empty() ? "(none - counter follows the MIDI)" : chartPath.c_str());

				if (ImGui::Button("Browse..."))
				{
					std::string path;
					if (Utils::ChooseFile(path, "cjcmcg"))
						app->LoadCounterSource(path.c_str());
				}

				ImGui::SameLine();
				ImGui::BeginDisabled(chartPath.empty());
				if (ImGui::Button("Clear"))
					app->ClearCounterSource();
				ImGui::EndDisabled();

				const CJCMCGChart* chart = app->GetCounterChart();
				if (chart != nullptr)
					ImGui::TextDisabled("%s notes over %s ticks",
						Utils::FormatWithCommas(chart->totalNotes).c_str(),
						Utils::FormatWithCommas((unsigned long long)chart->tickCount).c_str());
			}
		);

		END_SECTION;
	}
}

void SettingsDialog::DrawMultiplierTab()
{
	MIDIPlayerConfig* config = app->GetConfig();

	MIDISequence* seq = app->GetRendererSequence();
	const uint64_t totalNotes = seq != nullptr ? (uint64_t)std::max<long>(0L, seq->GetNotes()) : 0;
	const uint64_t melody = config->multiplier.melodyNotes;

	// Per-track note totals back the track list below and every projection in this tab. Counting
	// them walks all the note data, so they are cached until a different MIDI is loaded.
	const std::vector<uint64_t>& trackTotals = TrackNoteTotals(seq);

	// Notes kept out of the multiplication by the per-track selection.
	uint64_t excludedNotes = 0;
	for (uint16_t track : config->multiplier.unmultipliedTracks)
	{
		if (track < trackTotals.size())
			excludedNotes += trackTotals[track];
	}

	// Only the notes that are neither left out nor part of the melody get multiplied.
	const uint64_t appliedNotes = totalNotes > excludedNotes ? totalNotes - excludedNotes : 0;
	const uint64_t farmingNotes = appliedNotes > melody ? appliedNotes - melody : 0;

	if (ImGui::BeginChild("##multiplierScroll", ImVec2(0, 400), true, ImGuiWindowFlags_HorizontalScrollbar))
	{
		if (ImGui::IsWindowAppearing())
		{
			multiplierInputActive = false; // re-sync the custom field when the tab is reopened
			multiplierTargetActive = false;
		}

		ImGui::TextWrapped(
			"The note multiplier inflates the numbers shown by the note counter overlay. "
			"Actual notes, rendering and audio are left untouched - this is purely cosmetic."
		);
		ImGui::Spacing();
		ImGui::TextDisabled("displayed = (notes - melody) x multiplier + melody");
		ImGui::Spacing();

		BEGIN_SECTION("##multiplierSettings")
		{
			SETUP_SECTION;

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"Enable multiplier",
					"When disabled, the counter shows the real note, NPS and polyphony values."
				),
				ImGui::Checkbox("##multiplierEnabled", &config->multiplier.enabled);
			);

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"Multiplier",
					"Decimal factor applied to every note past the melody. Very large values (a googol and beyond) are supported."
				),
				{
					if (!multiplierInputActive)
						std::snprintf(multiplierInput, sizeof(multiplierInput), "%s", config->multiplier.value.c_str());

					// AutoSelectAll so the first keystroke replaces the value that is already in the box.
					// Without it the digits are merely inserted into it, which is how a two-digit edit can
					// turn a 13-digit multiplier into a 300-digit one.
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputText(
						"##customMultiplier",
						multiplierInput,
						sizeof(multiplierInput),
						ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll))
					{
						const std::string typed = Utils::NormalizeDecimalDigits(multiplierInput);
						if (typed.size() <= MAX_COUNTER_DIGITS)
							config->multiplier.value = typed;
					}

					multiplierInputActive = ImGui::IsItemActive();
				}
			);

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"Target note count",
					"Type the count you want the counter to reach. The multiplier above is solved for the currently loaded MIDI (rounded to the nearest whole factor)."
				),
				{
					// While the box isn't being edited it mirrors the count the current multiplier projects,
					// so it doubles as a preview of where the counter is heading.
					if (!multiplierTargetActive && totalNotes > 0)
					{
						const std::string projected = config->multiplier.enabled
							? NoteMultiplier::CountDigits(config, totalNotes, excludedNotes)
							: std::to_string(totalNotes);
						std::snprintf(multiplierTargetInput, sizeof(multiplierTargetInput), "%s", projected.c_str());
					}

					ImGui::BeginDisabled(farmingNotes == 0);

					// AutoSelectAll matters here: the box is pre-filled with a count that can be hundreds of
					// digits long, so without it typing a target only inserts those digits into the number
					// already sitting there (a 93,000,000 target once became a 306-digit multiplier that way).
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputTextWithHint(
						"##targetCount",
						"e.g. 93000000",
						multiplierTargetInput,
						sizeof(multiplierTargetInput),
						ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll))
					{
						const std::string target = ParseCount(multiplierTargetInput);

						if (target.empty())
						{
							// Box cleared: leave the multiplier alone until there is a target to solve for.
						}
						else if (target.size() > MAX_COUNTER_DIGITS)
						{
							// Refuse rather than solve for a count the counter could never display.
						}
						else
						{
							// displayed = (applied - melody) x multiplier + melody + excluded, so the factor that
							// reaches the typed target is (target - melody - excluded) / (applied - melody),
							// rounded to nearest.
							const std::string numerator = Utils::BigSubU64(Utils::BigSubU64(target, melody), excludedNotes);

							if (Utils::BigDivU64(numerator, farmingNotes, nullptr) == "0")
							{
								// The song is already at (or past) the target, so 1x is as low as it goes.
								config->multiplier.value = "1";
							}
							else
							{
								uint64_t remainder = 0;
								std::string factor = Utils::BigDivU64(numerator, farmingNotes, &remainder);
								if (remainder * 2 >= farmingNotes)
									factor = Utils::BigMulAddU64(factor, 1, 1);

								config->multiplier.value = factor;
								config->multiplier.enabled = factor != "1";
							}
						}
					}

					multiplierTargetActive = ImGui::IsItemActive();

					ImGui::EndDisabled();

					// Spell the solved multiplier out, so a surprise factor is obvious here instead of only
					// showing up as an unreadable number in the counter overlay.
					if (farmingNotes > 0)
					{
						const std::string target = ParseCount(multiplierTargetInput);
						const std::string reaches = config->multiplier.enabled
							? NoteMultiplier::CountDigits(config, totalNotes, excludedNotes)
							: std::to_string(totalNotes);

						// The box normally mirrors the projected count, and a projection is allowed to be huge;
					// only complain about text the user actually put there.
					const bool mirrorsProjection = multiplierTargetInput == reaches;

						if (!mirrorsProjection && target.size() > MAX_COUNTER_DIGITS)
						{
							ImGui::TextColored(WARNING_COLOR, "Too many digits - the counter can't reach past %zu.", MAX_COUNTER_DIGITS);
						}
						else if (!mirrorsProjection && !target.empty() && Utils::BigDivU64(Utils::BigSubU64(target, melody), farmingNotes, nullptr) == "0")
						{
							if (excludedNotes > 0)
								ImGui::TextColored(WARNING_COLOR, "%s of this MIDI's %s notes are left out of the multiplier, so that target is too low.", Utils::FormatWithCommas(excludedNotes).c_str(), Utils::FormatWithCommas(totalNotes).c_str());
							else
								ImGui::TextColored(WARNING_COLOR, "This MIDI already has %s notes, so 1x is as close as it gets.", Utils::FormatWithCommas(totalNotes).c_str());
						}
						else
						{
							ImGui::TextDisabled(
								"Multiplier %s  ->  counter reaches %s",
								PrettyDigits(config->multiplier.value).c_str(),
								PrettyDigits(reaches).c_str()
							);
						}
					}
					else if (totalNotes > 0)
					{
						ImGui::TextDisabled("Nothing to multiply: the tracks you kept are all within the melody.");
					}
				}
			);

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"Melody notes",
					"Leading notes that are NOT multiplied. NPS and polyphony also stay realistic while these are playing; only the farming notes (total - melody) get multiplied."
				),
				{
					int melodyValue = (int)config->multiplier.melodyNotes;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputInt("##melodyNotes", &melodyValue))
						config->multiplier.melodyNotes = (uint64_t)std::max(0, melodyValue);
				}
			);

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"NPS/Poly boost threshold",
					"After the melody has finished, only boost NPS and polyphony once they climb above this value (farming detected). 0 = always boost, whatever the note count. Notes from tracks you unticked below are never boosted, however high the rates get."
				),
				{
					int threshold = (int)config->multiplier.npsPolyBoostThreshold;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputInt("##npsPolyBoostThreshold", &threshold))
						config->multiplier.npsPolyBoostThreshold = (uint64_t)std::max(0, threshold);
				}
			);

			SECTION_ENTRY(
				TABLE_LABEL_TOOLTIP(
					"Multiply graph",
					"When on, the Graph overlay's NPS and polyphony lines are inflated by the multiplier too, "
					"so the graph matches the counter. Turn it off to keep the graph showing the real rates "
					"while the counter keeps farming."
				),
				ImGui::Checkbox("##multiplyGraph", &config->multiplier.multiplyGraph);
			);

			END_SECTION;
		}

		ImGui::Spacing();
		SECTION_HEADER("Tracks the multiplier applies to");

		if (seq == nullptr || trackTotals.empty())
		{
			ImGui::TextDisabled("Load a MIDI to choose which tracks get multiplied.");
		}
		else
		{
			ImGui::TextWrapped(
				"Untick a track and its notes are added back at 1x in every row - notes, NPS and "
				"polyphony - so a melody or a solo can stay realistic while the rest of the song runs away."
			);
			ImGui::Spacing();

			const std::size_t multipliedTracks = trackTotals.size() - std::min(trackTotals.size(), config->multiplier.unmultipliedTracks.size());
			ImGui::Text("Multiplied: %zu/%zu tracks", multipliedTracks, trackTotals.size());

			if (ImGui::SmallButton("Multiply all"))
				config->multiplier.unmultipliedTracks.clear();

			ImGui::SameLine();

			if (ImGui::SmallButton("Multiply none"))
			{
				config->multiplier.unmultipliedTracks.clear();
				for (std::size_t track = 0; track < trackTotals.size(); track++)
					config->multiplier.unmultipliedTracks.push_back((uint16_t)track);
			}

			ImGui::Spacing();

			BEGIN_SECTION("##multiplierTracks")
			{
				SETUP_SECTION;

				for (std::size_t track = 0; track < trackTotals.size(); track++)
				{
					const uint16_t trackIndex = (uint16_t)track;
					bool multiplied = config->multiplier.IsTrackMultiplied(trackIndex);

					const std::string name = seq != nullptr && track < seq->noteTrackNames.size()
						? seq->noteTrackNames[track]
						: std::string();

					const std::string label = TrackLabel(track, name);
					const std::string checkboxId = "##multiplyTrack" + std::to_string(track);

					SECTION_ENTRY(
						SECTION_LABEL(label.c_str()),
						{
							if (ImGui::Checkbox(checkboxId.c_str(), &multiplied))
								config->multiplier.SetTrackMultiplied(trackIndex, multiplied);

							ImGui::SameLine();
							ImGui::TextDisabled("%s notes", Utils::FormatWithCommas(trackTotals[track]).c_str());
						}
					);
				}

				END_SECTION;
			}
		}

		ImGui::Spacing();
		SECTION_HEADER("Multiplier presets");

		ImGui::Checkbox("Randomise preset values", &config->multiplier.randomizePresets);
		ImGui::SetItemTooltip(
			"Clicking a preset then hands back a random number of that size instead of a round 1 "
			"followed by zeros, so the counter reads like a real note count. Click a preset again to "
			"re-roll it."
		);

		ImGui::SameLine();

		if (ImGui::SmallButton("Re-roll"))
		{
			// Re-rolling keeps the current size, so the matching preset doesn't have to be hunted for.
			config->multiplier.value = Utils::RandomDecimalDigits(SignificantDigits(config->multiplier.value));
			config->multiplier.enabled = true;
		}
		ImGui::SetItemTooltip("Replace the multiplier with a fresh random number of the same size.");

		ImGui::Spacing();

		struct Preset { const char* name; std::string digits; bool randomizable; };
		static const Preset presets[] = {
			{ "65,522",              "65522",   true },
			{ "1 million",           Pow10(6),  true },
			{ "1 trillion",          Pow10(12), true },
			{ "10^20",               Pow10(20), true },
			{ "10^50",               Pow10(50), true },
			{ "googol (10^100)",     Pow10(100), true },
			{ "centillion (10^303)", Pow10(303), true },
			// Randomising the off switch would defeat the point of it.
			{ "Disabled (1)",        "1",        false },
		};

		for (const Preset& preset : presets)
		{
			const bool randomize = config->multiplier.randomizePresets && preset.randomizable;

			// A randomised value can't be matched exactly, so while randomising the preset whose size
			// the multiplier currently has stays highlighted.
			const bool isSelected = randomize
				? config->multiplier.value.size() == preset.digits.size()
				: config->multiplier.value == preset.digits;

			std::string label = preset.name;
			if (totalNotes > 0)
			{
				// Preview the size that is actually in the box when this is the selected one.
				const std::string shown = randomize && isSelected ? config->multiplier.value : preset.digits;
				const std::string projected = NoteMultiplier::CountDigitsWith(totalNotes, shown, melody, excludedNotes);
				const std::string pretty = projected.size() > 7
					? Utils::AbbreviateBigNumber(projected)
					: Utils::FormatWithCommas(projected);

				label += "     -> " + pretty;
			}

			if (ImGui::Selectable(label.c_str(), isSelected))
			{
				const std::string chosen = randomize
					? Utils::RandomDecimalDigits(preset.digits.size())
					: preset.digits;

				config->multiplier.value = chosen;
				config->multiplier.enabled = chosen != "1";
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (totalNotes > 0)
		{
			const std::string digits = NoteMultiplier::CountDigits(config, totalNotes, excludedNotes);
			ImGui::Text("Loaded MIDI: %s notes", Utils::FormatWithCommas(totalNotes).c_str());
			ImGui::Text("Counter will show: %s", Utils::FormatWithCommas(digits).c_str());
			ImGui::TextDisabled("(~%s)", Utils::AbbreviateBigNumber(digits).c_str());

			if (excludedNotes > 0)
				ImGui::TextDisabled("%s of those notes are left out of the multiplier.", Utils::FormatWithCommas(excludedNotes).c_str());
		}
		else
		{
			ImGui::TextDisabled("Load a MIDI to preview the resulting note count.");
		}
	}
	ImGui::EndChild();
}

const std::vector<uint64_t>& SettingsDialog::TrackNoteTotals(const MIDISequence* sequence)
{
	if (sequence == trackTotalsSequence)
		return trackNoteTotals;

	trackTotalsSequence = sequence;
	trackNoteTotals.clear();

	if (sequence == nullptr)
		return trackNoteTotals;

	// Every note carries its track index, so one pass over the per-key lists is enough.
	trackNoteTotals.assign(sequence->noteTrackCount, 0);

	for (const NoteSequence& notes : sequence->mergedNotes)
	{
		const std::size_t count = notes.Size();
		for (std::size_t i = 0; i < count; i++)
		{
			const uint16_t track = notes.GetTrack(i);
			if (track < trackNoteTotals.size())
				trackNoteTotals[track]++;
		}
	}

	return trackNoteTotals;
}