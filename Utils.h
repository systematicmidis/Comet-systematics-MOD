#pragma once

#include <array>
#include <string>
#include <sstream>
#include <iomanip>
#include <variant>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <algorithm>
#include <type_traits>
#include <chrono>
#include <filesystem>
#include <glm/glm.hpp>
#include <yaml-cpp/yaml.h>

struct ImVec4;

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Utils
{
	enum RGBAPackFormat
	{
		RGBA,
		ARGB,
		ABGR,
		BGRA
	};

	bool IsMIDIExtension(std::string extension);
	std::string FormatFilesize(long long bytes, int decimal);
	std::string FormatDuration(double ms);
	std::string FormatDuration2(double ms);
	std::optional<double> ParseTimeString(std::string timeString);
	bool ChooseFile(std::string& outPath, const char* extension = nullptr);
	bool SaveFile(std::string& outPath, const char* extension = nullptr);
	bool EqualsIgnoreCase(const std::string& a, const std::string& b);
	std::optional<ImVec4> ParseColor(std::variant<std::string, uint32_t> strOrInt, std::optional<ImVec4> def);
	std::string EncodeColor(ImVec4 color);
	uint32_t PackRGB(float r, float g, float b);
	std::array<float, 3> UnpackRGB(uint32_t rgb);
	uint32_t PackRGBA(float r, float g, float b, float a = 1.0f, RGBAPackFormat packFormat = RGBA);
	std::array<float, 4> UnpackRGBA(uint32_t rgba, RGBAPackFormat packFormat = RGBA);
	uint32_t BlendRGBA(uint32_t c1, uint32_t c2, float fac);
	uint32_t BlendRGBFast(uint32_t c1, uint32_t c2, uint32_t fac);
	static const std::string BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";
	std::string DecodeBase64(const std::string& encoded);
	std::filesystem::path GetExecutableDirectory();
	void OpenURL(const std::string& url);
	void OpenFolder(const std::string& path);
	bool FileExists(const std::string& path);
	bool FileExists(const std::wstring& path);
	bool FolderExists(const std::string& path);
	template <typename T>
	double GetCurrTime()
	{
		return (double)std::chrono::duration_cast<T>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	template <typename T>
	std::string FormatWithCommas(T value)
	{
		static_assert(std::is_integral_v<T>, "T must be an integer");

		std::string digits = std::to_string(value);

		const bool negative = digits[0] == '-';
		const std::size_t digitCount = digits.size() - negative;
		const std::size_t commaCount = (digitCount - 1) / 3;

		std::string result;
		result.resize(digits.size() + commaCount);

		std::size_t src = digits.size();
		std::size_t dst = result.size();
		int group = 0;

		while (src > static_cast<std::size_t>(negative))
		{
			result[--dst] = digits[--src];

			if (++group == 3 && src > static_cast<std::size_t>(negative))
			{
				result[--dst] = ',';
				group = 0;
			}
		}

		if (negative)
			result[0] = '-';

		return result;
	}
	// Strips everything that isn't a digit, then trims leading zeros. Empty / all-zero input
	// collapses to "1", which callers use as the "multiplier disabled" sentinel.
	std::string NormalizeDecimalDigits(const std::string& text);
    // A decimal string of exactly `digits` digits with no leading zero, used for multipliers that
    // should read like a real note count instead of a round 1 followed by zeros.
    std::string RandomDecimalDigits(size_t digits);
	// Multiplies a decimal digit string by a 64-bit factor and adds a 64-bit addend, exactly.
	// `factor` is expected to be a note count (< 2^31) so `digit * factor` can't overflow.
	std::string BigMulAddU64(const std::string& decimalDigits, uint64_t factor, uint64_t addend);
	// Divides a decimal digit string by a 64-bit divisor, returning the floored quotient as a digit
	// string. `remainder` (optional) receives the remainder so callers can round as they like.
	std::string BigDivU64(const std::string& decimalDigits, uint64_t divisor, uint64_t* remainder = nullptr);
	// Subtracts a 64-bit value from a decimal digit string, returning "0" if it isn't larger.
	std::string BigSubU64(const std::string& decimalDigits, uint64_t value);
	// Same grouping as the integer FormatWithCommas above, but for arbitrarily long digit strings.
	std::string FormatWithCommas(const std::string& decimalDigits);
	// Human readable form ("2.2 trillion"); falls back to scientific notation past vigintillion.
	std::string AbbreviateBigNumber(const std::string& decimalDigits);
	void AddFilePickerField(const char* label, std::filesystem::path& path, const char* extension, bool saving = false);
	
	YAML::Node Vec3ToNode(glm::vec3 vector);
	glm::vec3 NodeToVec3(const YAML::Node& node);
	YAML::Node Vec4ToNode(glm::vec4 vector);
	glm::vec4 NodeToVec4(const YAML::Node& node);

	std::shared_ptr<std::istream> TryGetStream(std::filesystem::path path);

	namespace Dialogs
	{
		enum DialogType
		{
			DIALOG_OK,
			DIALOG_WARNING,
			DIALOG_ERROR
		};

		static void ShowDialog(const char* title, const char* content, DialogType dialogType)
		{
#if defined(_WIN32)
			UINT uType = 0;
			switch (dialogType)
			{
			case DialogType::DIALOG_OK:
				uType = MB_ICONINFORMATION;
				break;
			case DialogType::DIALOG_ERROR:
				uType = MB_ICONERROR;
				break;
			case DialogType::DIALOG_WARNING:
				uType = MB_ICONWARNING;
				break;
			}

			MessageBoxA(nullptr, content, title, MB_OK | uType);
#else
			// no native message box on this platform, at least surface it on stderr
			std::fprintf(stderr, "[%s] %s\n", title, content);
			(void)dialogType;
#endif
		}
	}
}