#include "BannerRenderer.h"
#include "App/FontList.h"
#include "App/Fonts.h"
#include "App/MIDIApp.h"
#include "App/UI/Themes/Themes.h"
#include "Comet.h"
#include "Config/MIDIPlayerConfig.h"
#include "MIDI/Timer/MIDITimer.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace
{
	// Timings measured from the reference videos. UMP's card appears with the first frame, holds
	// until 2.80 s, then fades out over 0.20 s (the reference is fully gone at 3.00 s). Synthesia's
	// bar appears at once, holds, then fades out at 3.40 s and is gone by 3.65 s.
	constexpr double BANNER_SECONDS = 3.65;
	constexpr double UMP_FADE_START_SECONDS = 2.80;
	constexpr double UMP_FADE_SECONDS = 0.20;
	constexpr double UMP_SECONDS = UMP_FADE_START_SECONDS + UMP_FADE_SECONDS;
	constexpr double SYNTHESIA_FADE_OUT_START = 3.40;
	constexpr double SYNTHESIA_FADE_OUT_SECONDS = 0.25;
	constexpr double SYNTHESIA_TITLE_IN_START = 0.05;
	constexpr double SYNTHESIA_TITLE_IN_SECONDS = 0.45;
	constexpr double SYNTHESIA_TITLE_OUT_START = 2.72;
	constexpr double SYNTHESIA_TITLE_OUT_SECONDS = 0.45;

	// The preview starts its timer three seconds before the MIDI so the first notes have time to
	// fall. The "Now playing" card uses that pre-roll as its opening window.
	constexpr double PREVIEW_PREROLL_SECONDS = 3.0;

	// UMP's card is a white band 140 px high in a 1080p frame, centred vertically. The reference's
	// Comic Sans title/credit are 34 px/16.5 px faces; ImGui's bitmap font baker lays them out at
	// about 72.5% of the nominal size, so 47 px/26 px are requested to land on the measured ink.
	constexpr float UMP_BAND_HEIGHT_FRACTION = 140.0f / 1080.0f;
	constexpr float UMP_TITLE_SIZE_FRACTION = 47.0f / 1080.0f;
	constexpr float UMP_CREDIT_SIZE_FRACTION = 26.0f / 1080.0f;
	constexpr float UMP_CREDIT_INK_TOP_FRACTION = 37.0f / 1080.0f;
	constexpr float UMP_TITLE_INK_TOP_FRACTION = 73.0f / 1080.0f;
	// Ascender above cap height, as a fraction of the requested font size. Used to turn the
	// measured ink positions into the line-box origins ImGui's AddText expects.
	constexpr float TEXT_INK_OFFSET_FRACTION = 0.242f;

	// Synthesia's song title band, measured from "INSANE 88 KEY POLYRHYTHM" in a 1920 x 1080
	// window: the full-width bar spans y = 368..555, a 6 px bright edge plus a 6 px dark inner
	// shadow sit inside both ends, and the white title ink is 31 px high at y = 444. Those
	// measurements are converted to frame fractions here so the band keeps the same placement
	// at every render resolution.
	constexpr float SYNTHESIA_BAR_TOP_FRACTION = 368.0f / 1080.0f;
	constexpr float SYNTHESIA_BAR_HEIGHT_FRACTION = 188.0f / 1080.0f;
	constexpr float SYNTHESIA_TOP_INSET_FRACTION = 13.0f / 1080.0f;
	constexpr float SYNTHESIA_BOTTOM_INSET_FRACTION = 12.0f / 1080.0f;
	constexpr float SYNTHESIA_BORDER_FRACTION = 6.0f / 1080.0f;
	constexpr float SYNTHESIA_TITLE_SIZE_FRACTION = 52.0f / 1080.0f;
	constexpr float SYNTHESIA_TITLE_INK_TOP_FRACTION = 446.0f / 1080.0f;
	constexpr float SYNTHESIA_TITLE_OUTLINE_FRACTION = 2.0f / 1080.0f;
	constexpr float SYNTHESIA_TEXT_INK_OFFSET_FRACTION = 0.206f;

	struct SynthBarColors
	{
		ImU32 fill;
		ImU32 border;
		ImU32 shadow;
	};

	float Clamp01(float value)
	{
		return std::clamp(value, 0.0f, 1.0f);
	}

	float SmoothStep(float value)
	{
		value = Clamp01(value);
		return value * value * (3.0f - 2.0f * value);
	}

	float FadeOut(float time, double duration, double start, double fadeDuration)
	{
		if (time <= (float)start) return 1.0f;
		return Clamp01((float)(duration - time) / (float)fadeDuration);
	}

	float TextWidth(ImFont* font, float fontSize, const char* text)
	{
		return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text).x;
	}

	// Long song names should stay inside the frame instead of spilling off both edges.
	void FitFontSize(ImFont* font, float& fontSize, const char* text, float maxWidth)
	{
		const float width = TextWidth(font, fontSize, text);
		if (width > maxWidth && width > 0.0f)
			fontSize = std::max(1.0f, fontSize * (maxWidth / width));
	}

	void AddCenteredText(ImDrawList* draw, ImFont* font, float fontSize, float centerX, float topY,
		ImU32 color, const char* text)
	{
		const float width = TextWidth(font, fontSize, text);
		draw->AddText(font, fontSize, ImVec2(centerX - width * 0.5f, topY), color, text);
	}

	void AddOutlinedCenteredText(ImDrawList* draw, ImFont* font, float fontSize, float centerX,
		float topY, ImU32 textColor, ImU32 outlineColor, float outline, const char* text)
	{
		const float width = TextWidth(font, fontSize, text);
		const float left = centerX - width * 0.5f;

		for (int dx = -1; dx <= 1; ++dx)
		{
			for (int dy = -1; dy <= 1; ++dy)
			{
				if (dx == 0 && dy == 0) continue;
				draw->AddText(font, fontSize, ImVec2(left + dx * outline, topY + dy * outline),
					outlineColor, text);
			}
		}

		draw->AddText(font, fontSize, ImVec2(left, topY), textColor, text);
	}

	ImU32 WithAlpha(ImU32 color, float alpha)
	{
		const int a = (int)std::lround((float)((color >> IM_COL32_A_SHIFT) & 0xFF) * Clamp01(alpha));
		return (color & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
	}

	// Synthesia's title band has a light and a dark colour scheme. Comet's light theme uses the
	// Crystal Blue band from the reference recording; a dark theme uses the Synthesia Classic band
	// sampled from the 1080p "INSANE 88 KEY POLYRHYTHM" screenshot. Following the theme's
	// background instead of its accent keeps custom themes on the variant that matches their chrome.
	bool IsDarkTheme(const AppTheme* theme)
	{
		const ImVec4& background = theme->colors.background;
		const float luminance = background.x * 0.2126f + background.y * 0.7152f + background.z * 0.0722f;
		return luminance < 0.5f;
	}

	SynthBarColors GetSynthBarColors(MIDIApp* app)
	{
		bool dark = false;
		if (ThemesList* themes = app->GetThemeList())
		{
			if (AppTheme* theme = themes->GetCurrentTheme())
				dark = IsDarkTheme(theme);
		}

		if (dark)
		{
			// Synthesia Classic: fill #3F4654, bright edge #9CA4B8, inner shadow #24282E.
			return {
				IM_COL32(63, 70, 84, 255),
				IM_COL32(156, 164, 184, 255),
				IM_COL32(36, 40, 46, 255)
			};
		}

		// Crystal Blue: fill #107AAF, bright edge #00A8FF, inner shadow #094669.
		return {
			IM_COL32(16, 122, 175, 255),
			IM_COL32(0, 168, 255, 255),
			IM_COL32(9, 70, 105, 255)
		};
	}

	// The title as the reference banners show it: the file's name without its extension.
	std::string StripExtension(const std::string& name)
	{
		const std::filesystem::path path(name);
		const std::string stem = path.stem().string();
		return stem.empty() ? name : stem;
	}
}

void BannerRenderer::Reset()
{
}

BannerRenderer::Style BannerRenderer::GetStyle() const
{
	MIDIPlayerConfig* config = app->GetConfig();
	if (config->render.GetCurrentRenderer() == RendererType::Synthesia)
		return Style::Synthesia;

	return Style::UMP;
}

std::string BannerRenderer::GetTitle() const
{
	return StripExtension(app->GetLoadedMIDITitle());
}

ImFont* BannerRenderer::GetUMPFont() const
{
	// A font chosen for the counter and graph also sets the card's typeface. With no choice made,
	// use the app's default font so the UMP card and the Synthesia banner share one typeface.
	FontList* fontList = app->GetFontList();
	if (fontList != nullptr)
	{
		if (ImFont* selectedFont = fontList->GetFontForPath(app->GetConfig()->overlayInfo.selectedFontPath))
			return selectedFont;
	}

	return Fonts::DefaultFont;
}

void BannerRenderer::Render()
{
	if (app == nullptr || !app->hasSequence) return;

	MIDIPlayerConfig* config = app->GetConfig();
	if (!config->banner.enabled) return;

	ImGuiIO& io = ImGui::GetIO();
	const int width = (int)io.DisplaySize.x;
	const int height = (int)io.DisplaySize.y;
	if (width <= 0 || height <= 0) return;

	float time = 0.0f;
	if (app->IsRendering())
	{
		// The banner tracks the exported frame number instead of counting its own calls: the
		// render path draws one untimed settling frame first, and counting that frame made the
		// banner run ahead of the video by a frame or two.
		const int frame = app->GetCurrentFrame();
		if (frame < 0) return;

		const double fps = app->GetCurrentRenderSettings().fps > 0
			? (double)app->GetCurrentRenderSettings().fps
			: 60.0;
		time = (float)((double)frame / fps);
	}
	else
	{
		if (!config->banner.showInPreview) return;

		// The preview's pre-roll starts at -3 s, so the card appears as soon as a song is loaded
		// and finishes fading just as the MIDI begins.
		const double previewTime = app->GetTimer()->Elapsed() + PREVIEW_PREROLL_SECONDS;
		if (previewTime < 0.0) return;
		time = (float)previewTime;
	}

	if (time >= (float)BANNER_SECONDS) return;

	// Background still sits above the 3D scene, but ImGui windows (the menu bar, dialogs) draw
	// after it. That keeps the banner behind menus instead of painting over them.
	ImDrawList* draw = ImGui::GetBackgroundDrawList();
	if (GetStyle() == Style::Synthesia)
		DrawSynthesia(draw, width, height, time);
	else
		DrawUMP(draw, width, height, time);
}

void BannerRenderer::DrawUMP(ImDrawList* draw, int width, int height, float time) const
{
	if (time >= (float)UMP_SECONDS) return;

	// The reference card fades as one layer: the white band and its black text lose opacity
	// together from 2.80 s to 3.00 s.
	float alpha = 1.0f;
	if (time > (float)UMP_FADE_START_SECONDS)
		alpha = Clamp01(1.0f - (time - (float)UMP_FADE_START_SECONDS) / (float)UMP_FADE_SECONDS);
	if (alpha <= 0.0f) return;

	const ImU32 bandColor = IM_COL32(255, 255, 255, (int)std::lround(alpha * 255.0f));
	const ImU32 textColor = IM_COL32(0, 0, 0, (int)std::lround(alpha * 255.0f));

	const std::string title = GetTitle();
	ImFont* font = GetUMPFont();
	if (title.empty() || font == nullptr) return;

	// UMP's opening card: a full-width white band across the middle of the frame, with the credit
	// line above the song title in UMP's card face (or the selected overlay font, when one is set).
	const float bannerHeight = std::max(1.0f, (float)height * UMP_BAND_HEIGHT_FRACTION);
	const float bannerTop = ((float)height - bannerHeight) * 0.5f;

	draw->AddRectFilled(ImVec2(0.0f, bannerTop), ImVec2((float)width, bannerTop + bannerHeight),
		bandColor);

	float titleSize = (float)height * UMP_TITLE_SIZE_FRACTION;
	FitFontSize(font, titleSize, title.c_str(), (float)width * 0.92f);

	float creditSize = (float)height * UMP_CREDIT_SIZE_FRACTION;
	const std::string credit = app->IsRendering()
		? "Rendered on Systematics Comet MOD V" + std::string(COMET_VERSION)
		: "Now playing";
	FitFontSize(font, creditSize, credit.c_str(), (float)width * 0.92f);

	// Place the line boxes so the measured ink rows land where the reference has them.
	const float creditTop = bannerTop + (float)height * UMP_CREDIT_INK_TOP_FRACTION - creditSize * TEXT_INK_OFFSET_FRACTION;
	const float titleTop = bannerTop + (float)height * UMP_TITLE_INK_TOP_FRACTION - titleSize * TEXT_INK_OFFSET_FRACTION;

	AddCenteredText(draw, font, creditSize, (float)width * 0.5f, creditTop, textColor, credit.c_str());
	AddCenteredText(draw, font, titleSize, (float)width * 0.5f, titleTop, textColor, title.c_str());
}

void BannerRenderer::DrawSynthesia(ImDrawList* draw, int width, int height, float time) const
{
	const std::string title = GetTitle();
	// Synthesia's own UI typeface is closest to the app's default font.
	ImFont* font = Fonts::DefaultFont;
	if (title.empty() || font == nullptr) return;

	const float alpha = FadeOut(time, BANNER_SECONDS, SYNTHESIA_FADE_OUT_START, SYNTHESIA_FADE_OUT_SECONDS);
	if (alpha <= 0.0f) return;

	const SynthBarColors colors = GetSynthBarColors(app);
	const ImU32 fill = WithAlpha(colors.fill, alpha);
	const ImU32 border = WithAlpha(colors.border, alpha);
	const ImU32 shadow = WithAlpha(colors.shadow, alpha);

	// Scale the bar and title around the bar's vertical centre, keeping the reference position.
	// The bar stays full-width, as Synthesia's title bar does.
	const float scale = std::clamp(app->GetConfig()->banner.synthesiaScale,
		MIN_SYNTHESIA_BANNER_SCALE, MAX_SYNTHESIA_BANNER_SCALE);
	const float baseBarTop = (float)height * SYNTHESIA_BAR_TOP_FRACTION;
	const float baseBarHeight = std::max(1.0f, (float)height * SYNTHESIA_BAR_HEIGHT_FRACTION);
	const float barCenter = baseBarTop + baseBarHeight * 0.5f;
	const float barHeight = std::max(1.0f, baseBarHeight * scale);
	const float barTop = barCenter - barHeight * 0.5f;
	const float barBottom = barTop + barHeight;
	const float topInset = (float)height * SYNTHESIA_TOP_INSET_FRACTION * scale;
	const float bottomInset = (float)height * SYNTHESIA_BOTTOM_INSET_FRACTION * scale;
	const float borderHeight = std::max(1.0f, (float)height * SYNTHESIA_BORDER_FRACTION * scale);
	const float fillTop = barTop + topInset;
	const float fillBottom = barBottom - bottomInset;

	// Full-width, square-edged bar: a themed fill, a bright top and bottom edge, and a dark inner
	// shadow just inside each edge.
	draw->AddRectFilled(ImVec2(0.0f, fillTop), ImVec2((float)width, fillBottom), fill);
	draw->AddRectFilled(ImVec2(0.0f, barTop), ImVec2((float)width, barTop + borderHeight), border);
	draw->AddRectFilled(ImVec2(0.0f, barTop + borderHeight), ImVec2((float)width, fillTop), shadow);
	draw->AddRectFilled(ImVec2(0.0f, fillBottom), ImVec2((float)width, barBottom - borderHeight), shadow);
	draw->AddRectFilled(ImVec2(0.0f, barBottom - borderHeight), ImVec2((float)width, barBottom), border);

	if (time >= (float)(SYNTHESIA_TITLE_OUT_START + SYNTHESIA_TITLE_OUT_SECONDS)) return;

	float titleSize = (float)height * SYNTHESIA_TITLE_SIZE_FRACTION * scale;
	FitFontSize(font, titleSize, title.c_str(), (float)width * 0.92f);

	const float titleWidth = TextWidth(font, titleSize, title.c_str());
	const float fullOffset = (float)width * 0.5f + titleWidth * 0.5f;
	float titleCenterX = (float)width * 0.5f;

	if (time < (float)(SYNTHESIA_TITLE_IN_START + SYNTHESIA_TITLE_IN_SECONDS))
	{
		const float progress = SmoothStep((time - (float)SYNTHESIA_TITLE_IN_START) /
			(float)SYNTHESIA_TITLE_IN_SECONDS);
		titleCenterX += (1.0f - progress) * fullOffset;
	}
	else
	{
		const float progress = SmoothStep((time - (float)SYNTHESIA_TITLE_OUT_START) /
			(float)SYNTHESIA_TITLE_OUT_SECONDS);
		titleCenterX -= progress * fullOffset;
	}

	// The reference title is white with a black outline, not a drop shadow.
	const float titleInkTop = barCenter +
		((float)height * SYNTHESIA_TITLE_INK_TOP_FRACTION - barCenter) * scale;
	const float titleTop = titleInkTop - titleSize * SYNTHESIA_TEXT_INK_OFFSET_FRACTION;
	const float outline = std::max(1.0f, (float)height * SYNTHESIA_TITLE_OUTLINE_FRACTION * scale);

	AddOutlinedCenteredText(draw, font, titleSize, titleCenterX, titleTop,
		WithAlpha(IM_COL32(255, 255, 255, 255), alpha),
		WithAlpha(IM_COL32(0, 0, 0, 255), alpha),
		outline, title.c_str());
}
