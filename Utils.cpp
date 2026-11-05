#include "Utils.h"
#include <nfd.h>
#include <iostream>
#include <fstream>
#include <optional>
#include <vector>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "imgui.h"
#include <string_view>
#include <charconv>
#include <random>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__APPLE__)
    #include <cstdlib>
    #include <mach-o/dyld.h>
    #include <unistd.h>
    #include <sys/wait.h>
    #include <limits.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <limits.h>
    #include <cstdlib>
    #include <limits>
#endif

namespace Utils
{
	bool IsMIDIExtension(std::string extension)
	{
		return extension.compare("mid") || extension.compare("midi");
	}

    std::string FormatFilesize(long long bytes, int decimal)
    {
        static const char* units[] =
        {
            "B","KiB","MiB","GiB","TiB","PiB","EiB"
        };

        double d = (double)bytes;
        int i = 0;

        while (i < 6 && d >= 1024.0)
        {
            d /= 1024.0;
            i++;
        }

        std::ostringstream out;

        if (i == 0)
        {
            out << bytes << "B";
        }
        else
        {
            out << std::fixed << std::setprecision(decimal)
                << d << units[i];
        }

        return out.str();
    }

    std::string FormatDuration(double ms)
    {
        long totalSeconds = static_cast<long>(ms / 1000.0);

        long hours = totalSeconds / 3600;
        long minutes = (totalSeconds % 3600) / 60;
        long seconds = totalSeconds % 60;

        std::stringstream stream;

        if (hours > 0)
            stream << std::setfill('0') << std::setw(2) << hours << ":";

        stream << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << seconds;

        return stream.str();
    }

    std::string FormatDuration2(double ms)
    {
        if (ms < 0.0)
            ms = 0.0;

        long long totalTenths = static_cast<long long>(std::llround(ms / 100.0));
        long long totalSeconds = totalTenths / 10;
        long long tenths = totalTenths % 10;

        long long hours = totalSeconds / 3600;
        long long minutes = (totalSeconds % 3600) / 60;
        long long secs = totalSeconds % 60;

        std::ostringstream out;

        out << std::setfill('0');

        if (hours >= 10)
        {
            out << std::setw(2) << hours << ":"
                << std::setw(2) << minutes << ":"
                << std::setw(2) << secs << "."
                << tenths;
        }
        else if (hours > 0)
        {
            out << hours << ":"
                << std::setw(2) << minutes << ":"
                << std::setw(2) << secs << "."
                << tenths;
        }
        else if (minutes >= 10)
        {
            out << std::setw(2) << minutes << ":"
                << std::setw(2) << secs << "."
                << tenths;
        }
        else if (minutes > 0)
        {
            out << minutes << ":"
                << std::setw(2) << secs << "."
                << tenths;
        }
        else
        {
            out << secs << "."
                << tenths;
        }

        return out.str();
    }

    std::optional<double> ParseTimeString(std::string timeString)
    {
        double values[3]{};
        int count = 0;

        size_t start = 0;

        while (start < timeString.size() && count < 3)
        {
            size_t end = timeString.find(':', start);
            if (end == std::string_view::npos)
                end = timeString.size();

            std::string part = timeString.substr(start, end - start);

            double value{};
            auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);

            if (ec != std::errc{} || ptr != part.data() + part.size())
                return std::nullopt;

            values[count++] = value;

            if (end == timeString.size()) break;
            start = end + 1;
        }

        if (count == 3)
        {
            return std::optional(values[0] * 3600.0 + values[1] * 60.0 + values[2]);
        }
        else if (count == 2)
        {
            return std::optional(values[0] * 60.0 + values[1]);
        }
        else if (count == 1)
        {
            return std::optional(values[0]);
        }

        return std::nullopt;
    }

    bool ChooseFile(std::string& outPath, const char* extension)
    {
        nfdchar_t* outPathRaw = nullptr;
        auto result = NFD_OpenDialog(extension, nullptr, &outPathRaw);
        if (result == NFD_OKAY)
        {
            outPath = outPathRaw;
            free(outPathRaw);
            return true;
        }
        else if (result == NFD_CANCEL)
        {
            return false;
        }

        std::cerr << "Open file dialog failed: " << NFD_GetError() << std::endl;
        return false;
    }

    bool SaveFile(std::string& outPath, const char* extension)
    {
        nfdchar_t* outPathRaw = nullptr;
        auto result = NFD_SaveDialog(extension, nullptr, &outPathRaw);

        if (result == NFD_OKAY)
        {
            outPath = outPathRaw;
            free(outPathRaw);
            return true;
        }
        else if (result == NFD_CANCEL)
        {
            return false;
        }

        std::cerr << "Save file dialog failed: " << NFD_GetError() << std::endl;
        return false;
    }

    bool EqualsIgnoreCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::tolower(a[i]) != std::tolower(b[i])) return false;
        }
        return true;
    }

    std::optional<ImVec4> ParseColor(std::variant<std::string, uint32_t> strOrInt, std::optional<ImVec4> def)
    {
        const auto hex = [](char c) -> float {
            if (c >= '0' && c <= '9') return (c - '0') / 15.0f;
            if (c >= 'a' && c <= 'f') return (c - 'a' + 10) / 15.0f;
            if (c >= 'A' && c <= 'F') return (c - 'A' + 10) / 15.0f;
            return 0.0f;
        };

        if (std::holds_alternative<std::string>(strOrInt))
        {
            std::string str = std::get<std::string>(strOrInt);
            
            if (!str.empty() && (str[0] == '#' || str[0] == 'x' || str[0] == 'X'))
                str.erase(0, 1);
            else if (str.size() >= 2 && (str.starts_with("0x") || str.starts_with("0X")))
                str.erase(0, 2);

            if (str.size() == 3)
            {
                return ImVec4(
                    hex(str[0]),
                    hex(str[1]),
                    hex(str[2]),
                    1.0f
                );
            }

            if (str.size() == 4)
            {
                return ImVec4(
                    hex(str[1]),
                    hex(str[2]),
                    hex(str[3]),
                    hex(str[0])
                );
            }

            const auto parseByte = [&](int i) -> float {
                int hi = (str[i] >= '0' && str[i] <= '9') ? str[i] - '0'
                    : (str[i] >= 'a' && str[i] <= 'f') ? str[i] - 'a' + 10
                    : str[i] - 'A' + 10;

                int lo = (str[i + 1] >= '0' && str[i + 1] <= '9') ? str[i + 1] - '0'
                    : (str[i + 1] >= 'a' && str[i + 1] <= 'f') ? str[i + 1] - 'a' + 10
                    : str[i + 1] - 'A' + 10;

                return ((hi << 4) + lo) / 255.0f;
            };

            if (str.size() == 6)
                return ImVec4(parseByte(0), parseByte(2), parseByte(4), 1.0f);

            if (str.size() == 8)
                return ImVec4(parseByte(2), parseByte(4), parseByte(6), parseByte(0));

            return def;
        }
        else if (std::holds_alternative<uint32_t>(strOrInt))
        {
            uint32_t val = std::get<uint32_t>(strOrInt);
            return ImVec4(
                static_cast<float>((val & 0xFF0000) >> 16) / 255.0f,
                static_cast<float>((val & 0xFF00) >> 8) / 255.0f,
                static_cast<float>(val & 0xFF) / 255.0f,
                static_cast<float>((val & 0xFF000000) >> 24) / 255.0f
            );
        }
        return def;
    }

    std::string EncodeColor(ImVec4 color)
    {
        std::stringstream enc;

        enc << "0x";
        uint32_t red = static_cast<uint32_t>(std::round(color.x * 255));
        uint32_t green = static_cast<uint32_t>(std::round(color.y * 255));
        uint32_t blue = static_cast<uint32_t>(std::round(color.z * 255));
        uint32_t alpha = static_cast<uint32_t>(std::round(color.w * 255));

        red = std::clamp(red, 0u, 255u);
        green = std::clamp(green, 0u, 255u);
        blue = std::clamp(blue, 0u, 255u);
        alpha = std::clamp(alpha, 0u, 255u);

        uint32_t col = (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
        enc << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << col;

        return enc.str();
    }

    uint32_t PackRGB(float r, float g, float b)
    {
        return (uint32_t)(r * 255.0) |
            ((uint32_t)(g * 255.0) << 8u) |
            ((uint32_t)(b * 255.0) << 16u);
    }

    std::array<float, 3> UnpackRGB(uint32_t rgb)
    {
        float r = (float)(rgb & 0xFF) / 255.0f;
        float g = (float)((rgb >> 8) & 0xFF) / 255.0f;
        float b = (float)((rgb >> 16) && 0xFF) / 255.0f;
        return { r, g, b };
    }

    uint32_t PackRGBA(float r, float g, float b, float a, RGBAPackFormat format)
    {
        float c1, c2, c3, c4;
        switch (format)
        {
            case RGBA:
                c1 = r, c2 = g, c3 = b, c4 = a;
                break;
            case ARGB:
                c1 = a, c2 = r, c3 = g, c4 = b;
                break;
            case ABGR:
                c1 = a, c2 = b, c3 = g, c4 = r;
                break;
            case BGRA:
                c1 = b, c2 = g, c3 = r, c4 = a;
                break;
            default:
                return 0x00;
        }

        return (uint32_t)(c4 * 255.0) |
            ((uint32_t)(c3 * 255.0) << 8u) |
            ((uint32_t)(c2 * 255.0) << 16u) |
            ((uint32_t)(c1 * 255.0) << 24u);
    }

    std::array<float, 4> UnpackRGBA(uint32_t rgba, RGBAPackFormat format)
    {
        const float c4 = (rgba & 0xFF) / 255.0f;
        const float c3 = ((rgba >> 8) & 0xFF) / 255.0f;
        const float c2 = ((rgba >> 16) & 0xFF) / 255.0f;
        const float c1 = ((rgba >> 24) & 0xFF) / 255.0f;

        switch (format)
        {
        case RGBA:
            return { c1, c2, c3, c4 };

        case ARGB:
            return { c2, c3, c4, c1 };

        case ABGR:
            return { c4, c3, c2, c1 };

        case BGRA:
            return { c3, c2, c1, c4 };

        default:
            return {};
        }
    }

    uint32_t BlendRGBA(uint32_t c1, uint32_t c2, float fac)
    {
        std::array<float, 4> c1d = UnpackRGBA(c1);
        std::array<float, 4> c2d = UnpackRGBA(c2);
        std::array<float, 4> result{};
        for (int i = 0; i < 4; i++)
        {
            result[i] = (1.0 - fac) * c1d[i] + fac * c2d[i];
        }
        return PackRGBA(result[0], result[1], result[2], result[3]);
    }

    uint32_t BlendRGBFast(uint32_t c1, uint32_t c2, uint32_t fac)
    {
        uint32_t inv_t = 255 - fac;

        uint32_t rb1 = c1 & 0x00FF00FF;
        uint32_t rb2 = c2 & 0x00FF00FF;

        uint32_t ag1 = (c1 >> 8) & 0x00FF00FF;
        uint32_t ag2 = (c2 >> 8) & 0x00FF00FF;

        uint32_t rb = ((rb1 * inv_t + rb2 * fac) >> 8) & 0x00FF00FF;
        uint32_t ag = ((ag1 * inv_t + ag2 * fac)); 

        return rb | ((ag << 8) & 0xFF00FF00);
    }

    std::string DecodeBase64(const std::string& encoded)
    {
        std::vector<int> T(256, 1);
        for (int i = 0; i < 64; i++)
            T[BASE64_CHARS[i]] = i;

        std::string decoded;
        int val = 0, valb = -8;

        for (unsigned char c : encoded) {
            if (T[c] == -1) break;
            val = (val << 6) + T[c];
            valb += 6;
            if (valb >= 0) {
                decoded.push_back(char((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return decoded;
    }

    std::filesystem::path GetExecutableDirectory()
    {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD len = GetModuleFileNameW(NULL, buffer, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
            return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0)
            return std::filesystem::path(buffer).parent_path();
#else
        char buffer[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", buffer, sizeof(buffer));
        if (count > 0)
            return std::filesystem::path(std::string(buffer, count)).parent_path();
#endif
        return std::filesystem::path();
    }

    static void OpenWithDefaultHandler(const std::string& target)
    {
#if defined(_WIN32)
        int wlen = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, NULL, 0);
        if (wlen <= 0) return;
        std::wstring wide(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, &wide[0], wlen);
        ShellExecuteW(NULL, L"open", wide.c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
        const char* opener = "open";
#else
        const char* opener = "xdg-open";
#endif
        pid_t pid = fork();
        if (pid == 0)
        {
            // double fork so the opener is reparented and never left as a zombie
            if (fork() == 0)
            {
                execlp(opener, opener, target.c_str(), (char*)nullptr);
                _exit(127);
            }
            _exit(0);
        }
        else if (pid > 0)
        {
            int status = 0;
            waitpid(pid, &status, 0);
        }
        else
        {
            std::cerr << "Failed to launch " << opener << std::endl;
        }
#endif
    }

    void OpenURL(const std::string& url)
    {
        OpenWithDefaultHandler(url);
    }

    void OpenFolder(const std::string& path)
    {
        std::error_code ec;
        std::filesystem::path absPath = std::filesystem::absolute(std::filesystem::u8path(path), ec);
        if (ec)
        {
            std::cerr << "Failed to resolve path: " << ec.message() << std::endl;
            return;
        }

        auto u8 = absPath.u8string();
        OpenWithDefaultHandler(std::string(u8.begin(), u8.end()));
    }

    bool FolderExists(const std::string& path)
    {
        return FileExists(path) && std::filesystem::is_directory(path);
    }

    bool FileExists(const std::string& path)
    {
        return std::filesystem::exists(path);
    }

    bool FileExists(const std::wstring& path)
    {
        return std::filesystem::exists(path);
    }

    void AddFilePickerField(const char* label, std::filesystem::path& path, const char* extension, bool saving)
    {
        if (strlen(label) >= 1 && label[0] != '#')
        {
            ImGui::Text(label);
            ImGui::SameLine();
        }

        ImGui::PushID(label);
        if (ImGui::Button("Browse..."))
        {
            std::string filePath;
            bool result = saving ? Utils::SaveFile(filePath, extension) : Utils::ChooseFile(filePath, extension);
            if (result)
            {
#if defined(_WIN32) && defined(__cpp_lib_filesystem) && (__cplusplus >= 202002L || _MSVC_LANG >= 202002L)
                path = std::filesystem::path(std::u8string(filePath.begin(), filePath.end()));
#else
                path = std::filesystem::u8path(filePath);
#endif
            }
        }

        ImGui::SameLine();

        auto u8str = path.u8string();
        std::string buf(u8str.begin(), u8str.end());
        ImGui::InputText("##pickedFilePath", buf.data(), buf.size(), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopID();
    }

    YAML::Node Vec3ToNode(glm::vec3 vector)
    {
        YAML::Node node;
        node["x"] = vector.x;
        node["y"] = vector.y;
        node["z"] = vector.z;
        return node;
    }

    glm::vec3 NodeToVec3(const YAML::Node& node)
    {
        glm::vec3 vector{};
        vector.x = node["x"].as<float>();
        vector.y = node["y"].as<float>();
        vector.z = node["z"].as<float>();
        return vector;
    }

    YAML::Node Vec4ToNode(glm::vec4 vector)
    {
        YAML::Node node;
        node["x"] = vector.x;
        node["y"] = vector.y;
        node["z"] = vector.z;
        node["w"] = vector.w;
        return node;
    }

    glm::vec4 NodeToVec4(const YAML::Node& node)
    {
        glm::vec4 vector{};
        vector.x = node["x"].as<float>();
        vector.y = node["y"].as<float>();
        vector.z = node["z"].as<float>();
        vector.w = node["w"].as<float>();
        return vector;
    }

    std::shared_ptr<std::istream> TryGetStream(std::filesystem::path path)
    {
        if (!std::filesystem::exists(path) ||
            !std::filesystem::is_regular_file(path))
            return nullptr;

        auto stream = std::make_shared<std::ifstream>(path, std::ios::binary);

        if (!stream->is_open())
            return nullptr;

        return stream;
    }

    std::string NormalizeDecimalDigits(const std::string& text)
    {
        std::string digits;
        digits.reserve(text.size());

        for (char c : text)
        {
            if (c >= '0' && c <= '9')
                digits.push_back(c);
        }

        const std::size_t firstNonZero = digits.find_first_not_of('0');
        if (firstNonZero == std::string::npos)
            return "1"; // empty or all zeros -> multiplier disabled

        return digits.substr(firstNonZero);
    }

    std::string RandomDecimalDigits(size_t digits)
    {
        if (digits == 0)
            digits = 1;

        std::random_device device;
        std::mt19937 generator(device());
        std::uniform_int_distribution<int> leading(1, 9);
        std::uniform_int_distribution<int> anyDigit(0, 9);

        std::string result;
        result.reserve(digits);
        result.push_back(static_cast<char>('0' + leading(generator)));

        for (size_t i = 1; i < digits; i++)
            result.push_back(static_cast<char>('0' + anyDigit(generator)));

        return result;
    }

    std::string BigMulAddU64(const std::string& decimalDigits, uint64_t factor, uint64_t addend)
    {
        // Grade-school litte-endian accumulation. `factor` is a note count (well below 2^32),
        // so `digit * factor + carry` stays comfortably inside 64 bits.
        uint64_t carry = addend;
        std::string result;
        result.reserve(decimalDigits.size() + 24);

        for (auto it = decimalDigits.rbegin(); it != decimalDigits.rend(); ++it)
        {
            if (*it < '0' || *it > '9') continue;

            const uint64_t value = static_cast<uint64_t>(*it - '0') * factor + carry;
            result.push_back(static_cast<char>('0' + (value % 10ull)));
            carry = value / 10ull;
        }

        while (carry > 0)
        {
            result.push_back(static_cast<char>('0' + (carry % 10ull)));
            carry /= 10ull;
        }

        std::reverse(result.begin(), result.end());

        const std::size_t firstNonZero = result.find_first_not_of('0');
        if (firstNonZero == std::string::npos)
            return "0";

        return result.substr(firstNonZero);
    }

    std::string BigDivU64(const std::string& decimalDigits, uint64_t divisor, uint64_t* remainder)
    {
        if (remainder) *remainder = 0;
        if (divisor == 0) return "0";

        // Grade-school long division on the digit string; `divisor` is a note count (< 2^32), so
        // `remainder * 10 + digit` can't overflow 64 bits.
        std::string quotient;
        quotient.reserve(decimalDigits.size());
        uint64_t rem = 0;

        for (char c : decimalDigits)
        {
            if (c < '0' || c > '9') continue;

            rem = rem * 10ull + static_cast<uint64_t>(c - '0');
            quotient.push_back(static_cast<char>('0' + (rem / divisor)));
            rem %= divisor;
        }

        if (remainder) *remainder = rem;

        const std::size_t firstNonZero = quotient.find_first_not_of('0');
        if (firstNonZero == std::string::npos) return "0";
        return quotient.substr(firstNonZero);
    }

    std::string BigSubU64(const std::string& decimalDigits, uint64_t value)
    {
        std::string digits;
        digits.reserve(decimalDigits.size());
        for (char c : decimalDigits)
        {
            if (c >= '0' && c <= '9') digits.push_back(c);
        }

        const std::size_t firstNonZero = digits.find_first_not_of('0');
        if (firstNonZero == std::string::npos) return "0";
        digits = digits.substr(firstNonZero);

        const std::string valueText = std::to_string(value);
        if (digits.size() < valueText.size() ||
            (digits.size() == valueText.size() && digits <= valueText))
            return "0";

        int i = static_cast<int>(digits.size()) - 1;
        int j = static_cast<int>(valueText.size()) - 1;
        int borrow = 0;

        while (i >= 0 && (j >= 0 || borrow > 0))
        {
            const int subtrahend = (j >= 0 ? valueText[j] - '0' : 0) + borrow;
            int diff = (digits[i] - '0') - subtrahend;
            if (diff < 0) { diff += 10; borrow = 1; }
            else { borrow = 0; }

            digits[i] = static_cast<char>('0' + diff);
            --i; --j;
        }

        const std::size_t trimmed = digits.find_first_not_of('0');
        if (trimmed == std::string::npos) return "0";
        return digits.substr(trimmed);
    }

    std::string FormatWithCommas(const std::string& decimalDigits)
    {
        if (decimalDigits.empty())
            return decimalDigits;

        const bool negative = decimalDigits[0] == '-';
        const std::string body = decimalDigits.substr(negative ? 1 : 0);

        std::string result;
        result.reserve(body.size() + body.size() / 3 + (negative ? 1 : 0));

        int group = 0;
        for (std::size_t i = body.size(); i-- > 0; )
        {
            result.push_back(body[i]);

            if (++group == 3 && i > 0)
            {
                result.push_back(',');
                group = 0;
            }
        }

        if (negative) result.push_back('-');

        std::reverse(result.begin(), result.end());
        return result;
    }

    std::string AbbreviateBigNumber(const std::string& decimalDigits)
    {
        const std::size_t firstNonZero = decimalDigits.find_first_not_of('0');
        if (firstNonZero == std::string::npos)
            return "0";

        const std::string digits = decimalDigits.substr(firstNonZero);
        if (digits.size() <= 4)
            return digits; // too small to abbreviate

        static const char* const NAMES[] = {
            "", "thousand", "million", "billion", "trillion", "quadrillion",
            "quintillion", "sextillion", "septillion", "octillion", "nonillion",
            "decillion", "undecillion", "duodecillion", "tredecillion", "quattuordecillion",
            "quindecillion", "sexdecillion", "septendecillion", "octodecillion",
            "novemdecillion", "vigintillion"
        };

        const std::size_t exponent = digits.size() - 1;
        const std::size_t group = exponent / 3;

        std::string mantissa = digits.substr(0, std::min<std::size_t>(4, digits.size()));

        if (group >= sizeof(NAMES) / sizeof(NAMES[0]))
        {
            // Scientific notation: always one digit before the decimal point.
            if (mantissa.size() > 1)
            {
                mantissa.insert(mantissa.begin() + 1, '.');
                while (mantissa.size() > 2 && mantissa.back() == '0') mantissa.pop_back();
                if (!mantissa.empty() && mantissa.back() == '.') mantissa.pop_back();
            }

            return mantissa + "e" + std::to_string(exponent);
        }

        // The decimal point sits (exponent % 3) digits after the first significant digit, so
        // 12345 -> "12.34 thousand" while 1000000 -> "1 million".
        const std::size_t dotPos = exponent % 3 + 1;
        if (mantissa.size() > dotPos)
        {
            mantissa.insert(mantissa.begin() + dotPos, '.');
            while (mantissa.size() > dotPos + 1 && mantissa.back() == '0') mantissa.pop_back();
            if (!mantissa.empty() && mantissa.back() == '.') mantissa.pop_back();
        }

        return mantissa + " " + NAMES[group];
    }
}