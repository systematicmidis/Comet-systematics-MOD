#include "Telemetry/CrashReporter.h"

#include "Telemetry/CometLog.h"

#include "Comet.h"

// Platform headers first: <windows.h> defines macros that break the C++ headers if it lands after
// them. NOMINMAX keeps its min/max macros out of the way.
#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
	#include <dbghelp.h>
	#include <intrin.h>
	#pragma comment(lib, "dbghelp.lib")
#else
	#include <unistd.h>
	#if defined(__has_include)
		#if __has_include(<execinfo.h>)
			#include <execinfo.h>
			#define COMET_HAS_EXECINFO 1
		#endif
	#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace CrashReporter
{
	namespace
	{
		using json = nlohmann::json;

		constexpr int SCHEMA_VERSION = 1;
		constexpr int MAX_STACK_FRAMES = 64;
		constexpr int LOG_LINES_IN_REPORT = 150;
		// Discord truncates longer descriptions outright, so cut it ourselves and say so.
		constexpr size_t EMBED_DESCRIPTION_LIMIT = 3900;
		constexpr size_t EMBED_FIELD_LIMIT = 960;
		constexpr size_t MAX_ATTACHMENT_BYTES = 7u * 1024u * 1024u;
		// A single run shouldn't hammer a webhook that is rate limiting us.
		constexpr int MAX_UPLOADS_PER_FLUSH = 10;
		constexpr int CURL_TIMEOUT_SECONDS = 15;
		constexpr int CURL_CONNECT_TIMEOUT_SECONDS = 8;

		// ---------------------------------------------------------------------------------------
		// App-supplied context. Guarded by a mutex, but the crash handler uses try_lock: if another
		// thread died holding it, the report simply goes out with fewer fields.
		// ---------------------------------------------------------------------------------------
		std::mutex g_contextMutex;
		MidiInfo g_midi;
		RuntimeFlags g_flags;
		std::string g_settingsSnapshot = "{}";
		std::string g_gpuVendor;
		std::string g_gpuRenderer;
		std::string g_gpuVersion;
		std::string g_glVersion;
		std::string g_version = COMET_VERSION;
		std::string g_buildType = "Unknown";
		std::string g_buildConfig = "";
		// Defaults to the compiled-in endpoint; only the test harness overrides it via SetWebhookUrl.
		std::string g_webhookUrl = DISCORD_WEBHOOK_URL;
		bool g_enabled = false;

		bool g_installed = false;
		std::chrono::steady_clock::time_point g_startTime;
		// Set before any report is written; a second fatal error while reporting just dies.
		std::atomic<bool> g_reporting{ false };
		std::terminate_handler g_previousTerminate = nullptr;

#if defined(_WIN32)
		using UnhandledFilter = LPTOP_LEVEL_EXCEPTION_FILTER;
		UnhandledFilter g_previousFilter = nullptr;
		bool g_symbolsReady = false;
#endif

		// ---------------------------------------------------------------------------------------
		// Small helpers
		// ---------------------------------------------------------------------------------------
		std::string Hex(uint64_t value, int width)
		{
			std::ostringstream out;
			out << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value;
			return out.str();
		}

		std::string UtcTimestamp()
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t asTime = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
		#if defined(_WIN32)
			gmtime_s(&tm, &asTime);
		#else
			gmtime_r(&asTime, &tm);
		#endif
			std::ostringstream out;
			out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
			return out.str();
		}

		std::string LocalTimestamp()
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t asTime = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
		#if defined(_WIN32)
			localtime_s(&tm, &asTime);
		#else
			localtime_r(&asTime, &tm);
		#endif
			std::ostringstream out;
			out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
			return out.str();
		}

		std::string StampForId()
		{
			const auto now = std::chrono::system_clock::now();
			const std::time_t asTime = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
		#if defined(_WIN32)
			gmtime_s(&tm, &asTime);
		#else
			gmtime_r(&asTime, &tm);
		#endif
			std::ostringstream out;
			out << std::put_time(&tm, "%Y%m%d-%H%M%S");
			return out.str();
		}

		std::string Truncate(const std::string& text, size_t limit)
		{
			if (text.size() <= limit)
				return text;

			const std::string suffix = "\n... (truncated)";
			if (limit <= suffix.size())
				return text.substr(0, limit);

			return text.substr(0, limit - suffix.size()) + suffix;
		}

		// Discord renders this inside a ``` fence, so a stray fence in the payload would end the
		// code block early and mangle the message.
		std::string SanitizeForCodeBlock(const std::string& text)
		{
			std::string out = text;
			for (char& c : out)
			{
				if (c == '`')
					c = '\'';
			}
			return out;
		}

		// An embed field may not be empty, and a zero-length value is rejected by Discord.
		std::string FieldValue(const std::string& value)
		{
			if (value.empty())
				return "(unknown)";
			return Truncate(value, EMBED_FIELD_LIMIT);
		}

		double UptimeSeconds()
		{
			const auto elapsed = std::chrono::steady_clock::now() - g_startTime;
			return std::chrono::duration<double>(elapsed).count();
		}

		std::string FormatUptime(double seconds)
		{
			const long long total = static_cast<long long>(seconds);
			std::ostringstream out;
			if (total >= 3600)
				out << (total / 3600) << "h " << ((total % 3600) / 60) << "m " << (total % 60) << "s";
			else if (total >= 60)
				out << (total / 60) << "m " << (total % 60) << "s";
			else
				out << total << "s";
			return out.str();
		}

#if defined(_WIN32)
		std::string Utf8FromWide(const std::wstring& wide)
		{
			if (wide.empty())
				return {};

			const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
				nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return {};

			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
				out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring WideFromUtf8(const std::string& text)
		{
			if (text.empty())
				return {};

			const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				nullptr, 0);
			if (size <= 0)
				return {};

			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
				out.data(), size);
			return out;
		}

		bool ReadEnvWide(const wchar_t* name, std::wstring& out)
		{
			const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
			if (needed == 0)
				return false;

			std::wstring buffer(needed, L'\0');
			const DWORD written = GetEnvironmentVariableW(name, buffer.data(), needed);
			if (written == 0 || written >= needed)
				return false;

			buffer.resize(written);
			out = buffer;
			return true;
		}

		std::wstring ExecutablePathW()
		{
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
					static_cast<DWORD>(buffer.size()));
				if (written == 0)
					return {};

				if (written < buffer.size() - 1)
				{
					buffer.resize(written);
					return buffer;
				}

				buffer.resize(buffer.size() * 2);
			}
		}

		std::wstring CurrentDirectoryW()
		{
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD written = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
				if (written == 0)
					return {};

				if (written < buffer.size())
				{
					buffer.resize(written);
					return buffer;
				}

				buffer.resize(written + 1);
			}
		}

		// Stable name for the binary that owns `address`, plus its load base.
		std::string ModuleForAddress(uint64_t address, uint64_t* baseOut)
		{
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(address)), &module) || !module)
			{
				return "<unknown>";
			}

			if (baseOut)
				*baseOut = reinterpret_cast<uint64_t>(module);

			std::wstring path(MAX_PATH, L'\0');
			const DWORD written = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			if (written == 0)
				return "<unknown>";

			path.resize(written);
			std::error_code ec;
			const std::filesystem::path asPath(path);
			return Utf8FromWide(asPath.filename().wstring());
		}

		struct ExceptionDescription
		{
			const char* name;
			const char* description;
			const char* hint;
		};

		bool DescribeException(DWORD code, ExceptionDescription& out)
		{
			switch (code)
			{
				case 0xC0000005:
					out = { "EXCEPTION_ACCESS_VIOLATION",
						"The thread tried to read from or write to a virtual address it does not have access to.",
						"Usually a null or already-freed pointer. Check the faulting module and the frame closest to the top of the stack." };
					return true;
				case 0xC000001D:
					out = { "EXCEPTION_ILLEGAL_INSTRUCTION", "The thread tried to execute an invalid instruction.", nullptr };
					return true;
				case 0xC0000025:
					out = { "EXCEPTION_NONCONTINUABLE_EXCEPTION", "The thread tried to continue execution after a noncontinuable exception.", nullptr };
					return true;
				case 0xC000008C:
					out = { "EXCEPTION_ARRAY_BOUNDS_EXCEEDED", "The thread tried to access an array element that is out of bounds.", nullptr };
					return true;
				case 0xC000008D:
					out = { "EXCEPTION_FLT_DENORMAL_OPERAND", "A floating-point operand was a denormal value.", nullptr };
					return true;
				case 0xC000008E:
					out = { "EXCEPTION_FLT_DIVIDE_BY_ZERO", "The thread tried to divide a floating-point value by zero.", nullptr };
					return true;
				case 0xC000008F:
					out = { "EXCEPTION_FLT_INEXACT_RESULT", "A floating-point operation could not be represented exactly.", nullptr };
					return true;
				case 0xC0000090:
					out = { "EXCEPTION_FLT_INVALID_OPERATION", "A floating-point operation performed an invalid operation.", nullptr };
					return true;
				case 0xC0000091:
					out = { "EXCEPTION_FLT_OVERFLOW", "The exponent of a floating-point operation exceeded its range.", nullptr };
					return true;
				case 0xC0000092:
					out = { "EXCEPTION_FLT_STACK_CHECK", "The stack was corrupted as a result of a floating-point operation.", nullptr };
					return true;
				case 0xC0000093:
					out = { "EXCEPTION_FLT_UNDERFLOW", "The exponent of a floating-point operation fell below its range.", nullptr };
					return true;
				case 0xC0000094:
					out = { "EXCEPTION_INT_DIVIDE_BY_ZERO", "The thread tried to divide an integer value by zero.", "Look for a divisor derived from the loaded MIDI (length, tick count, tempo) being zero." };
					return true;
				case 0xC0000095:
					out = { "EXCEPTION_INT_OVERFLOW", "The result of an integer operation caused a carry out of the most significant bit.", nullptr };
					return true;
				case 0xC0000096:
					out = { "EXCEPTION_PRIV_INSTRUCTION", "The thread tried to execute an instruction whose operation is not allowed in the current machine mode.", nullptr };
					return true;
				case 0xC00000FD:
					out = { "EXCEPTION_STACK_OVERFLOW", "The thread used up its stack.", "Often unbounded recursion. The stack trace below may be incomplete because there was no room to capture it." };
					return true;
				case 0xC0000374:
					out = { "STATUS_HEAP_CORRUPTION", "The heap was found to be corrupt when it was walked.", "Something wrote past the end of an allocation. The stack trace points at the check, not necessarily the culprit." };
					return true;
				case 0xC0000409:
					out = { "STATUS_STACK_BUFFER_OVERRUN", "A buffer overrun was detected and the process was fast-failed.", "The /GS cookie tripped, so a fixed-size buffer was overflowed." };
					return true;
				case 0xC0000602:
					out = { "STATUS_FAIL_FAST_EXCEPTION", "The process was fast-failed on purpose.", nullptr };
					return true;
				case 0xC0000135:
					out = { "STATUS_DLL_NOT_FOUND", "A DLL required to start or continue could not be found.", "A dependency (bass.dll, bassmidi.dll, the MSVC runtime, a resource pack plugin) is missing next to Comet.exe." };
					return true;
				case 0xC0000139:
					out = { "STATUS_ENTRYPOINT_NOT_FOUND", "A DLL was found but does not export a procedure that was called.", "A stale or mixed-version DLL is next to the executable." };
					return true;
				case 0xC0000142:
					out = { "STATUS_DLL_INIT_FAILED", "A DLL failed to initialise.", "Often a missing runtime or a driver DLL refusing to load. Check what changed most recently." };
					return true;
				case 0x80000003:
					out = { "EXCEPTION_BREAKPOINT", "A breakpoint was hit with no debugger attached.", nullptr };
					return true;
				case 0xE06D7363:
					out = { "MSVC_CPP_EXCEPTION", "An unhandled C++ exception reached the top of the stack.", "The message below is the exception's what(); the report type says whether it was std::terminate." };
					return true;
				default:
					return false;
			}
		}

		// One frame as a report line: index, address, owning module, then the symbol and source
		// location when a .pdb could be found.
		std::string FormatFrame(int index, uint64_t address)
		{
			uint64_t moduleBase = 0;
			const std::string module = ModuleForAddress(address, &moduleBase);

			std::ostringstream line;
			line << "#" << std::setw(2) << std::setfill('0') << index << " "
				<< Hex(address, 16) << "  " << module;

			if (moduleBase != 0)
				line << "+" << Hex(address - moduleBase, 0);

			if (!g_symbolsReady)
				return line.str();

			const HANDLE process = GetCurrentProcess();

			// Sized up front so no allocation happens per frame while the process is dying.
			char symbolStorage[sizeof(SYMBOL_INFO) + 512] = {};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;

			DWORD64 displacement = 0;
			if (SymFromAddr(process, address, &displacement, symbol))
			{
				line << "  " << symbol->Name;
				if (displacement != 0)
					line << "+" << Hex(displacement, 0);
			}

			IMAGEHLP_LINE64 lineInfo{};
			lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			DWORD lineDisplacement = 0;
			if (SymGetLineFromAddr64(process, address, &lineDisplacement, &lineInfo) && lineInfo.FileName)
			{
				const std::string file = Utf8FromWide(
					std::filesystem::path(lineInfo.FileName).filename().wstring());
				line << "  (" << file << ":" << lineInfo.LineNumber << ")";
			}

			return line.str();
		}

		// Walks the faulting thread's stack from the exception's own instruction pointer. This is
		// what puts the crashing function at frame #00; capturing from here instead would make the
		// top of the trace this handler, which is exactly the frame nobody cares about.
		std::vector<std::string> CaptureStackTraceFromException(EXCEPTION_POINTERS* info, int maxFrames)
		{
			std::vector<std::string> frames;

			if (!info || !info->ContextRecord || !info->ExceptionRecord)
				return frames;

			CONTEXT context = *info->ContextRecord;
			STACKFRAME64 frame{};

		#if defined(_M_IX86)
			const DWORD machine = IMAGE_FILE_MACHINE_I386;
			frame.AddrPC.Offset = context.Eip;
			frame.AddrFrame.Offset = context.Ebp;
			frame.AddrStack.Offset = context.Esp;
		#else
			const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
			frame.AddrPC.Offset = context.Rip;
			frame.AddrFrame.Offset = context.Rbp;
			frame.AddrStack.Offset = context.Rsp;
		#endif

			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Mode = AddrModeFlat;

			const HANDLE process = GetCurrentProcess();
			const HANDLE thread = GetCurrentThread();

			for (int i = 0; i < maxFrames; ++i)
			{
				if (!StackWalk64(machine, process, thread, &frame, &context, nullptr,
						SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
					break;

				if (frame.AddrPC.Offset == 0)
					break;

				frames.push_back(FormatFrame(i, frame.AddrPC.Offset));
			}

			return frames;
		}

		// Fallback for reports that are requested rather than caused by a crash, where there is no
		// exception record to walk from.
		std::vector<std::string> CaptureStackTrace(int maxFrames)
		{
			std::vector<std::string> frames;

			void* addresses[MAX_STACK_FRAMES] = {};
			const int limit = maxFrames < MAX_STACK_FRAMES ? maxFrames : MAX_STACK_FRAMES;
			// Skip this function so the trace starts at its caller.
			const USHORT captured = CaptureStackBackTrace(1, static_cast<DWORD>(limit), addresses, nullptr);

			for (USHORT i = 0; i < captured; ++i)
				frames.push_back(FormatFrame(i, reinterpret_cast<uint64_t>(addresses[i])));

			if (frames.empty())
				frames.push_back("(no frames captured)");

			return frames;
		}
#else
		std::vector<std::string> CaptureStackTrace(int maxFrames)
		{
			std::vector<std::string> frames;

		#if defined(COMET_HAS_EXECINFO)
			void* addresses[MAX_STACK_FRAMES] = {};
			const int limit = maxFrames < MAX_STACK_FRAMES ? maxFrames : MAX_STACK_FRAMES;
			const int captured = backtrace(addresses, limit);
			for (int i = 0; i < captured; ++i)
			{
				std::ostringstream line;
				line << "#" << std::setw(2) << std::setfill('0') << i << " " << addresses[i];
				frames.push_back(line.str());
			}
		#endif

			if (frames.empty())
				frames.push_back("(no frames captured)");

			return frames;
		}
#endif

		// ---------------------------------------------------------------------------------------
		// Where reports live
		// ---------------------------------------------------------------------------------------
#if defined(_WIN32)
		std::wstring ReportDirectoryW()
		{
			std::wstring override;
			if (ReadEnvWide(L"COMET_CRASH_REPORT_DIR", override) && !override.empty())
				return override;

			std::wstring localAppData;
			if (ReadEnvWide(L"LOCALAPPDATA", localAppData) && !localAppData.empty())
			{
				std::filesystem::path dir(localAppData);
				dir /= L"Comet";
				dir /= L"crash_reports";
				return dir.wstring();
			}

			// Last resort: beside the executable. May be read-only under Program Files, in which
			// case the report simply fails to write and we fall back to the log.
			std::filesystem::path exeDir(ExecutablePathW());
			exeDir = exeDir.parent_path();
			exeDir /= L"crash_reports";
			return exeDir.wstring();
		}
#endif

		std::filesystem::path ReportDirectoryPath()
		{
		#if defined(_WIN32)
			return std::filesystem::path(ReportDirectoryW());
		#else
			if (const char* overrideDir = std::getenv("COMET_CRASH_REPORT_DIR"))
			{
				if (*overrideDir != '\0')
					return std::filesystem::path(overrideDir);
			}

			if (const char* home = std::getenv("HOME"))
				return std::filesystem::path(home) / ".local" / "share" / "Comet" / "crash_reports";

			return std::filesystem::current_path() / "crash_reports";
		#endif
		}

		bool EnsureDirectory(const std::filesystem::path& dir)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
				return true;

			std::filesystem::create_directories(dir, ec);
			return !ec;
		}

		std::filesystem::path PendingDirectory() { return ReportDirectoryPath() / "pending"; }
		std::filesystem::path SentDirectory() { return ReportDirectoryPath() / "sent"; }
		std::filesystem::path FailedDirectory() { return ReportDirectoryPath() / "failed"; }
		std::filesystem::path LastErrorPath() { return ReportDirectoryPath() / "last_error.txt"; }

		void WriteLastError(const std::string& message)
		{
			if (message.empty())
				return;

			if (!EnsureDirectory(ReportDirectoryPath()))
				return;

			std::error_code ec;
			std::ofstream out(LastErrorPath(), std::ios::binary | std::ios::trunc);
			if (!out.is_open())
				return;

			out << "[" << UtcTimestamp() << "] " << message << "\n";
		}

		bool WriteTextFile(const std::filesystem::path& path, const std::string& contents)
		{
		#if defined(_WIN32)
			const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE)
				return false;

			const char* cursor = contents.data();
			size_t remaining = contents.size();

			while (remaining > 0)
			{
				const DWORD chunk = remaining > (1u << 20) ? (1u << 20) : static_cast<DWORD>(remaining);
				DWORD written = 0;
				if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0)
				{
					CloseHandle(handle);
					return false;
				}

				cursor += written;
				remaining -= written;
			}

			CloseHandle(handle);
			return true;
		#else
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
				return false;

			out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
			out.flush();
			return out.good();
		#endif
		}

		bool ReadTextFile(const std::filesystem::path& path, std::string& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in.is_open())
				return false;

			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		// ---------------------------------------------------------------------------------------
		// System information
		// ---------------------------------------------------------------------------------------
		json CollectSystemInfo()
		{
			json system = json::object();

		#if defined(_WIN32)
			struct OsVersionInfo
			{
				ULONG size;
				ULONG major;
				ULONG minor;
				ULONG build;
				ULONG platformId;
				wchar_t csdVersion[128];
			};

			// GetVersionEx lies without an application manifest, so ask ntdll instead.
			OsVersionInfo version{};
			version.size = sizeof(version);

			using RtlGetVersionFn = LONG(WINAPI*)(OsVersionInfo*);
			if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
			{
				if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
						GetProcAddress(ntdll, "RtlGetVersion")))
				{
					rtlGetVersion(&version);
				}
			}

			std::string product = "Windows";
			if (version.build >= 22000)
				product = "Windows 11";
			else if (version.build >= 10240)
				product = "Windows 10";
			else if (version.build >= 9200)
				product = "Windows 8/8.1";
			else if (version.build >= 7600)
				product = "Windows 7";

			std::ostringstream os;
			os << product << " " << version.major << "." << version.minor << "." << version.build;
			if (version.csdVersion[0] != L'\0')
				os << " " << Utf8FromWide(version.csdVersion);

			system["os"] = os.str();

			SYSTEM_INFO sysInfo{};
			GetNativeSystemInfo(&sysInfo);

			const char* architecture = "Unknown";
			switch (sysInfo.wProcessorArchitecture)
			{
				case PROCESSOR_ARCHITECTURE_AMD64: architecture = "x64"; break;
				case PROCESSOR_ARCHITECTURE_INTEL: architecture = "x86"; break;
				case PROCESSOR_ARCHITECTURE_ARM64: architecture = "ARM64"; break;
				case PROCESSOR_ARCHITECTURE_ARM:   architecture = "ARM"; break;
				default: break;
			}

			system["architecture"] = architecture;
			system["pointerSize"] = static_cast<int>(sizeof(void*)) * 8;
			system["logicalCores"] = static_cast<int>(sysInfo.dwNumberOfProcessors);

			// Processor brand string. Leaves 0x80000002..0x80000004 are exactly three 16-byte
			// chunks; the buffer size is derived from the leaf count so the copy can never run past
			// the end, whatever that count is.
			constexpr int BRAND_LEAVES = 3;
			constexpr int BRAND_BYTES = BRAND_LEAVES * 16;

			int registers[4] = {};
			char brand[BRAND_BYTES + 1] = {};

			for (int leaf = 0; leaf < BRAND_LEAVES; ++leaf)
			{
				__cpuid(registers, 0x80000002 + leaf);
				std::memcpy(brand + leaf * 16, registers, sizeof(registers));
			}

			// The last leaf is not guaranteed to be zero-padded, so terminate it explicitly.
			brand[BRAND_BYTES] = '\0';

			std::string brandText = brand;
			while (!brandText.empty() && brandText.front() == ' ')
				brandText.erase(brandText.begin());
			while (!brandText.empty() && brandText.back() == ' ')
				brandText.pop_back();

			system["cpu"] = brandText.empty() ? "(unknown)" : brandText;

			MEMORYSTATUSEX memory{};
			memory.dwLength = sizeof(memory);
			if (GlobalMemoryStatusEx(&memory))
			{
				system["physicalMemoryMb"] = static_cast<long long>(memory.ullTotalPhys / (1024ull * 1024ull));
				system["availableMemoryMb"] = static_cast<long long>(memory.ullAvailPhys / (1024ull * 1024ull));
				system["memoryLoadPercent"] = static_cast<int>(memory.dwMemoryLoad);
			}

			system["screen"] = std::to_string(GetSystemMetrics(SM_CXSCREEN)) + "x" +
				std::to_string(GetSystemMetrics(SM_CYSCREEN));

			wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
			if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
				system["locale"] = Utf8FromWide(locale);

			TIME_ZONE_INFORMATION zone{};
			if (GetTimeZoneInformation(&zone) != TIME_ZONE_ID_INVALID)
				system["timezone"] = Utf8FromWide(zone.StandardName);

			if (!g_gpuRenderer.empty() || !g_gpuVendor.empty())
			{
				json gpu = json::object();
				gpu["vendor"] = g_gpuVendor;
				gpu["renderer"] = g_gpuRenderer;
				gpu["driverVersion"] = g_gpuVersion;
				gpu["openglVersion"] = g_glVersion;
				system["gpu"] = gpu;
			}
		#else
			system["os"] = "Non-Windows";
			system["architecture"] = static_cast<int>(sizeof(void*)) * 8 == 64 ? "64-bit" : "32-bit";
			system["logicalCores"] = static_cast<int>(std::thread::hardware_concurrency());
			if (!g_gpuRenderer.empty())
				system["gpu"] = { { "vendor", g_gpuVendor }, { "renderer", g_gpuRenderer } };
		#endif

			return system;
		}

		json CollectEnvironment()
		{
			json environment = json::object();
			environment["utcTime"] = UtcTimestamp();
			environment["localTime"] = LocalTimestamp();
			environment["uptimeSeconds"] = UptimeSeconds();
			environment["uptimePretty"] = FormatUptime(UptimeSeconds());

		#if defined(_WIN32)
			environment["processId"] = static_cast<long long>(GetCurrentProcessId());
			environment["threadId"] = static_cast<long long>(GetCurrentThreadId());
			environment["executablePath"] = Utf8FromWide(ExecutablePathW());
			environment["workingDirectory"] = Utf8FromWide(CurrentDirectoryW());
			environment["reportDirectory"] = Utf8FromWide(ReportDirectoryW());

			wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
			DWORD computerSize = MAX_COMPUTERNAME_LENGTH + 1;
			if (GetComputerNameW(computer, &computerSize))
				environment["computerName"] = Utf8FromWide(computer);

			wchar_t user[256] = {};
			DWORD userSize = 256;
			if (GetUserNameW(user, &userSize))
				environment["userName"] = Utf8FromWide(user);

			environment["sessionId"] = static_cast<long long>(0);
			DWORD sessionId = 0;
			if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId))
				environment["sessionId"] = static_cast<long long>(sessionId);
		#else
			environment["processId"] = static_cast<long long>(getpid());
			environment["workingDirectory"] = std::filesystem::current_path().string();
		#endif

			return environment;
		}

		json CollectApplicationInfo()
		{
			json application = json::object();
			application["name"] = "Comet";
			application["version"] = g_version;
			application["buildType"] = g_buildType;
			application["buildConfig"] = g_buildConfig;

		#if defined(_MSC_VER)
			application["compiler"] = std::string("MSVC ") + std::to_string(_MSC_VER);
		#elif defined(__clang__)
			application["compiler"] = std::string("Clang ") + __clang_version__;
		#elif defined(__GNUC__)
			application["compiler"] = std::string("GCC ") + __VERSION__;
		#else
			application["compiler"] = "Unknown";
		#endif

			application["buildDate"] = std::string(__DATE__) + " " + __TIME__;

			// A beta suffix (1.9.0b) is what the strings in the About box show.
			std::string pretty = g_version;
			if (!pretty.empty() && pretty.back() == 'b')
			{
				pretty.pop_back();
				pretty += "-Beta";
			}
			application["versionPretty"] = pretty;

			return application;
		}

		json CollectState()
		{
			json state = json::object();

			MidiInfo midi;
			RuntimeFlags flags;
			bool haveContext = false;

			{
				// Never block a dying process: if another thread died holding this, skip the block.
				std::unique_lock<std::mutex> lock(g_contextMutex, std::try_to_lock);
				if (lock.owns_lock())
				{
					midi = g_midi;
					flags = g_flags;
					haveContext = true;
				}
			}

			state["contextAvailable"] = haveContext;

			if (haveContext)
			{
				json midiJson = json::object();
				midiJson["path"] = midi.path;
				midiJson["displayName"] = midi.displayName;
				midiJson["notes"] = midi.notes;
				midiJson["lengthTicks"] = midi.lengthTicks;
				midiJson["trackCount"] = midi.trackCount;
				midiJson["noteTrackCount"] = midi.noteTrackCount;
				midiJson["resolution"] = midi.resolution;
				midiJson["lengthSeconds"] = midi.lengthSeconds;
				state["midi"] = midiJson;

				state["hasSequence"] = flags.hasSequence;
				state["playing"] = flags.playing;
				state["rendering"] = flags.rendering;
				state["loading"] = flags.loading;
			}

			return state;
		}

		json CollectSettings()
		{
			std::string snapshot;
			{
				std::unique_lock<std::mutex> lock(g_contextMutex, std::try_to_lock);
				if (lock.owns_lock())
					snapshot = g_settingsSnapshot;
			}

			if (snapshot.empty())
				return json::object();

			const json parsed = json::parse(snapshot, nullptr, false);
			if (parsed.is_discarded())
				return json::object();

			// The webhook URL is a credential; it must never travel inside the report itself.
			if (parsed.is_object() && parsed.contains("crashReports"))
			{
				json copy = parsed;
				copy["crashReports"].erase("webhookUrl");
				return copy;
			}

			return parsed;
		}

		json CollectLogs()
		{
			json logs = json::object();

			const std::string tail = CometLog::Tail(LOG_LINES_IN_REPORT);
			json lines = json::array();

			size_t start = 0;
			while (start <= tail.size())
			{
				const size_t newline = tail.find('\n', start);
				if (newline == std::string::npos)
					break;

				lines.push_back(tail.substr(start, newline - start));
				start = newline + 1;
			}

			logs["lineCount"] = static_cast<int>(lines.size());
			logs["lines"] = std::move(lines);
			return logs;
		}

		std::string MintReportId()
		{
		#if defined(_WIN32)
			std::ostringstream out;
			out << StampForId() << "-" << std::hex << std::uppercase
				<< static_cast<unsigned long>(GetCurrentProcessId() & 0xFFFF) << "-"
				<< static_cast<unsigned long>(GetTickCount64() & 0xFFFF);
			return out.str();
		#else
			std::ostringstream out;
			out << StampForId() << "-" << static_cast<long long>(getpid());
			return out.str();
		#endif
		}

		// Describes *why* we are reporting; empty for a deliberate test-send.
		struct Fault
		{
			std::string type;        // "seh", "terminate", "signal", "none"
			std::string code;
			std::string name;
			std::string description;
			std::string hint;
			std::string address;
			std::string module;
			std::string threadId;
			std::string accessType;
			std::string accessAddress;
			std::string detail;
			bool hasFault = false;
		};

		json FaultToJson(const Fault& fault)
		{
			json out = json::object();

			if (!fault.hasFault)
			{
				out["type"] = "none";
				out["description"] = "No exception: this report was requested from the app rather than produced by a crash.";
				return out;
			}

			out["type"] = fault.type;
			out["code"] = fault.code;
			out["name"] = fault.name;
			out["description"] = fault.description;
			if (!fault.hint.empty())
				out["hint"] = fault.hint;
			if (!fault.address.empty())
				out["address"] = fault.address;
			if (!fault.module.empty())
				out["module"] = fault.module;
			out["threadId"] = fault.threadId;
			if (!fault.accessType.empty())
				out["accessType"] = fault.accessType;
			if (!fault.accessAddress.empty())
				out["accessAddress"] = fault.accessAddress;
			if (!fault.detail.empty())
				out["detail"] = fault.detail;

			return out;
		}

		std::string BuildReportInternal(const std::string& kind, const Fault& fault,
			const std::vector<std::string>& stackTrace, const std::string& note)
		{
			json report = json::object();

			report["schema"] = SCHEMA_VERSION;
			report["kind"] = kind;
			report["reportId"] = MintReportId();
			report["createdUtc"] = UtcTimestamp();
			report["createdLocal"] = LocalTimestamp();
			report["application"] = CollectApplicationInfo();
			report["system"] = CollectSystemInfo();
			report["environment"] = CollectEnvironment();
			report["exception"] = FaultToJson(fault);
			report["stackTrace"] = stackTrace;
			report["state"] = CollectState();
			report["settings"] = CollectSettings();
			report["logs"] = CollectLogs();

			if (!note.empty())
				report["note"] = note;

			// Consumed and stripped by the sender; never uploaded as part of the report.
			std::string endpoint;
			{
				std::unique_lock<std::mutex> lock(g_contextMutex, std::try_to_lock);
				if (lock.owns_lock())
					endpoint = g_webhookUrl;
			}

			report["_delivery"] = {
				{ "transport", "discord-webhook" },
				{ "endpoint", endpoint }
			};

			return report.dump(2);
		}

		// ---------------------------------------------------------------------------------------
		// Delivery
		// ---------------------------------------------------------------------------------------
		enum class Delivery
		{
			Ok,
			// The endpoint rejected the report; retrying will never help.
			Permanent,
			// Network hiccup or rate limit; keep it queued.
			Transient
		};

		size_t CurlWriteCallback(char* data, size_t size, size_t count, void* userData)
		{
			auto* out = static_cast<std::string*>(userData);
			out->append(data, size * count);
			return size * count;
		}

		Delivery PostJson(const std::string& url, const std::string& payload,
			std::string& responseOut, std::string& errorOut)
		{
			CURL* curl = curl_easy_init();
			if (!curl)
			{
				errorOut = "could not create a libcurl handle";
				return Delivery::Transient;
			}

			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "Comet-CrashReporter");
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(CURL_TIMEOUT_SECONDS));
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(CURL_CONNECT_TIMEOUT_SECONDS));
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseOut);

			curl_slist* headers = nullptr;
			headers = curl_slist_append(headers, "Content-Type: application/json");
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));

			const CURLcode result = curl_easy_perform(curl);

			long httpCode = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);

			if (result != CURLE_OK)
			{
				errorOut = std::string("network error: ") + curl_easy_strerror(result);
				return Delivery::Transient;
			}

			if (httpCode == 200 || httpCode == 201 || httpCode == 204)
				return Delivery::Ok;

			if (httpCode == 429 || httpCode >= 500)
			{
				errorOut = "server said HTTP " + std::to_string(httpCode) + " (will retry): " +
					Truncate(responseOut, 300);
				return Delivery::Transient;
			}

			if (httpCode >= 400)
			{
				errorOut = "endpoint rejected the report with HTTP " + std::to_string(httpCode) + ": " +
					Truncate(responseOut, 300);
				return Delivery::Permanent;
			}

			errorOut = "unexpected HTTP " + std::to_string(httpCode);
			return Delivery::Transient;
		}

		// Multipart POST: Discord wants the embed as JSON and the raw report as a file.
		Delivery PostMultipart(const std::string& url, const std::string& payloadJson,
			const std::string& attachmentName, const std::string& attachmentBody,
			std::string& responseOut, std::string& errorOut)
		{
			CURL* curl = curl_easy_init();
			if (!curl)
			{
				errorOut = "could not create a libcurl handle";
				return Delivery::Transient;
			}

			curl_mime* mime = curl_mime_init(curl);

			curl_mimepart* payloadPart = curl_mime_addpart(mime);
			curl_mime_name(payloadPart, "payload_json");
			curl_mime_type(payloadPart, "application/json");
			curl_mime_data(payloadPart, payloadJson.c_str(), CURL_ZERO_TERMINATED);

			curl_mimepart* filePart = curl_mime_addpart(mime);
			curl_mime_name(filePart, "files[0]");
			curl_mime_type(filePart, "application/json");
			curl_mime_filename(filePart, attachmentName.c_str());
			curl_mime_data(filePart, attachmentBody.c_str(), CURL_ZERO_TERMINATED);

			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_USERAGENT, "Comet-CrashReporter");
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(CURL_TIMEOUT_SECONDS));
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(CURL_CONNECT_TIMEOUT_SECONDS));
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
			curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteCallback);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseOut);

			const CURLcode result = curl_easy_perform(curl);

			long httpCode = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

			curl_mime_free(mime);
			curl_easy_cleanup(curl);

			if (result != CURLE_OK)
			{
				errorOut = std::string("network error: ") + curl_easy_strerror(result);
				return Delivery::Transient;
			}

			if (httpCode == 200 || httpCode == 201 || httpCode == 204)
				return Delivery::Ok;

			if (httpCode == 429 || httpCode >= 500)
			{
				errorOut = "Discord said HTTP " + std::to_string(httpCode) + " (will retry): " +
					Truncate(responseOut, 300);
				return Delivery::Transient;
			}

			if (httpCode >= 400)
			{
				errorOut = "Discord rejected the report with HTTP " + std::to_string(httpCode) + ": " +
					Truncate(responseOut, 300);
				return Delivery::Permanent;
			}

			errorOut = "unexpected HTTP " + std::to_string(httpCode);
			return Delivery::Transient;
		}

		// Pulls the endpoint out of a report's "_delivery" block and removes it, so the credential
		// is never part of what gets uploaded.
		bool ExtractEndpoint(std::string& reportJson, std::string& endpointOut)
		{
			json document = json::parse(reportJson, nullptr, false);
			if (document.is_discarded() || !document.is_object() || !document.contains("_delivery"))
				return false;

			const json& delivery = document["_delivery"];
			if (delivery.is_object() && delivery.contains("endpoint") && delivery["endpoint"].is_string())
				endpointOut = delivery["endpoint"].get<std::string>();

			document.erase("_delivery");
			reportJson = document.dump(2);
			return !endpointOut.empty();
		}

		std::string StackTraceText(const json& report, size_t limit)
		{
			std::string text;
			if (report.contains("stackTrace") && report["stackTrace"].is_array())
			{
				for (const auto& frame : report["stackTrace"])
				{
					if (!frame.is_string())
						continue;

					text += frame.get<std::string>();
					text += '\n';
				}
			}

			if (text.empty())
				text = "(no stack trace)";

			return Truncate(SanitizeForCodeBlock(text), limit);
		}
	}

	// -------------------------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------------------------
	std::string ReportDirectory()
	{
	#if defined(_WIN32)
		return Utf8FromWide(ReportDirectoryW());
	#else
		return ReportDirectoryPath().string();
	#endif
	}

	void SetAppInfo(std::string version, std::string buildType, std::string buildConfig)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_version = std::move(version);
		g_buildType = std::move(buildType);
		g_buildConfig = std::move(buildConfig);
	}

	void SetGraphicsInfo(std::string vendor, std::string renderer, std::string version, std::string glVersion)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_gpuVendor = std::move(vendor);
		g_gpuRenderer = std::move(renderer);
		g_gpuVersion = std::move(version);
		g_glVersion = std::move(glVersion);
	}

	void SetMidiInfo(const MidiInfo& info)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_midi = info;
	}

	void ClearMidiInfo()
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_midi = MidiInfo{};
	}

	void SetRuntimeFlags(const RuntimeFlags& flags)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_flags = flags;
	}

	void SetSettingsSnapshot(std::string json)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_settingsSnapshot = std::move(json);
	}

	void SetEnabled(bool enabled)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_enabled = enabled;
	}

	void SetWebhookUrl(std::string url)
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		g_webhookUrl = std::move(url);
	}

	bool IsEnabled()
	{
		std::lock_guard<std::mutex> lock(g_contextMutex);
		return g_enabled;
	}

	std::string BuildReport(const std::string& kind, const std::string& note)
	{
		Fault fault{};
		// A requested report still carries this thread's stack, which makes the test send a real
		// check of the symbolication path.
		return BuildReportInternal(kind, fault, CaptureStackTrace(32), note);
	}

	std::string BuildDiscordPayload(const std::string& reportJson)
	{
		const json report = json::parse(reportJson, nullptr, false);
		if (report.is_discarded() || !report.is_object())
			return "{}";

		const std::string kind = report.value("kind", "crash");
		const bool isCrash = kind == "crash";

		const json& exception = report.contains("exception") ? report["exception"] : json::object();
		const json& application = report.contains("application") ? report["application"] : json::object();
		const json& system = report.contains("system") ? report["system"] : json::object();
		const json& environment = report.contains("environment") ? report["environment"] : json::object();

		std::string title = "Comet " + application.value("version", "?") + " ";
		title += isCrash ? "crashed" : "report";

		if (isCrash && exception.is_object() && exception.contains("name"))
			title += ": " + exception.value("name", "");

		json embed = json::object();
		// Discord's red, for crashes; amber for requested reports.
		embed["color"] = isCrash ? 0xED4245 : 0xFEE75C;
		embed["title"] = Truncate(title, 256);

		const std::string stackText = StackTraceText(report, EMBED_DESCRIPTION_LIMIT);
		std::string description;

		if (isCrash)
			description = "```\n" + stackText + "```";
		else
			description = report.value("note", std::string("Report requested from the app."));

		if (!isCrash)
			description += "\n\n**Stack trace**\n```\n" + stackText + "```";

		embed["description"] = Truncate(description, 4050);

		json fields = json::array();
		auto addField = [&fields](const std::string& name, const std::string& value, bool inlineField)
		{
			if (value.empty())
				return;

			fields.push_back({
				{ "name", Truncate(name, 250) },
				{ "value", FieldValue(Truncate(value, EMBED_FIELD_LIMIT)) },
				{ "inline", inlineField }
			});
		};

		if (isCrash && exception.is_object())
		{
			addField("Exception", exception.value("code", "") + " " + exception.value("name", ""), true);
			if (exception.contains("module"))
				addField("Faulting module", exception["module"].get<std::string>(), true);
			addField("Thread", exception.value("threadId", ""), true);

			if (exception.contains("hint"))
				addField("Likely cause", exception["hint"].get<std::string>(), false);

			if (exception.contains("detail"))
				addField("Detail", exception["detail"].get<std::string>(), false);
		}

		addField("Version", application.value("versionPretty", "") + "  (" +
			application.value("buildType", "") + " " + application.value("buildConfig", "") + ")", true);
		addField("OS", system.value("os", "") + "  " + system.value("architecture", ""), true);
		addField("Uptime", environment.value("uptimePretty", ""), true);

		if (system.contains("gpu") && system["gpu"].is_object())
			addField("GPU", system["gpu"].value("renderer", ""), false);

		if (report.contains("state") && report["state"].is_object())
		{
			const json& state = report["state"];
			if (state.contains("midi") && state["midi"].is_object())
			{
				const json& midi = state["midi"];
				std::ostringstream midiText;
				midiText << midi.value("displayName", "");
				if (midi.contains("notes"))
					midiText << "  (" << midi.value("notes", 0LL) << " notes";
				if (midi.contains("trackCount"))
					midiText << ", " << midi.value("trackCount", 0) << " tracks";

				std::string text = midiText.str();
				if (text.find('(') != std::string::npos)
					text += ")";

				if (!midi.value("path", std::string()).empty())
					text += "\n`" + midi.value("path", std::string()) + "`";

				addField("Loaded MIDI", text, false);
			}

			std::ostringstream flagText;
			flagText << (state.value("playing", false) ? "playing" : "stopped")
				<< ", " << (state.value("hasSequence", false) ? "sequence loaded" : "no sequence")
				<< ", " << (state.value("rendering", false) ? "rendering video" : "not rendering");

			addField("State", flagText.str(), false);
		}

		if (report.contains("logs") && report["logs"].is_object())
		{
			// The last few log lines are usually the most useful thing in the whole report.
			const json& lines = report["logs"].value("lines", json::array());
			std::string tail;
			const size_t take = 12;
			const size_t begin = lines.size() > take ? lines.size() - take : 0;
			for (size_t i = begin; i < lines.size(); ++i)
			{
				if (!lines[i].is_string())
					continue;

				tail += lines[i].get<std::string>();
				tail += '\n';
			}

			if (!tail.empty())
				addField("Last log lines", "```\n" + SanitizeForCodeBlock(Truncate(tail, EMBED_FIELD_LIMIT - 20)) + "```", false);
		}

		embed["fields"] = fields;
		embed["footer"] = { { "text", "report " + report.value("reportId", std::string("?")) } };
		embed["timestamp"] = report.value("createdUtc", UtcTimestamp());

		json payload = json::object();
		payload["username"] = "Comet Crash Reporter";
		payload["embeds"] = json::array({ embed });

		return payload.dump();
	}

	bool PostToDiscord(const std::string& webhookUrl, const std::string& reportJson, std::string* errorOut)
	{
		std::string error;

		if (webhookUrl.empty())
		{
			if (errorOut)
				*errorOut = "no webhook URL is configured";

			return false;
		}

		if (!webhookUrl.starts_with("https://") && !webhookUrl.starts_with("http://"))
		{
			if (errorOut)
				*errorOut = "the webhook URL must start with https://";

			return false;
		}

		std::string body = reportJson;
		std::string ignoredEndpoint;
		// Strip the credential (and the "_delivery" block) before anything leaves the machine.
		ExtractEndpoint(body, ignoredEndpoint);

		const std::string payload = BuildDiscordPayload(body);

		const std::string attachmentName = "crash_report.json";
		const bool attachFile = body.size() < MAX_ATTACHMENT_BYTES;

		std::string response;
		Delivery delivery = attachFile
			? PostMultipart(webhookUrl, payload, attachmentName, body, response, error)
			: PostJson(webhookUrl, payload, response, error);

		if (delivery != Delivery::Ok && errorOut)
			*errorOut = error;

		return delivery == Delivery::Ok;
	}

	bool SendOrQueue(const std::string& reportJson, const std::string& webhookUrl, std::string* errorOut)
	{
		const json report = json::parse(reportJson, nullptr, false);
		const std::string reportId = report.is_object()
			? report.value("reportId", std::string("unknown"))
			: std::string("unknown");

		// The id is used in the filename; keep it to characters that are safe everywhere.
		std::string safeId;
		for (char c : reportId)
		{
			if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
				c == '-' || c == '_')
				safeId += c;
		}

		if (safeId.empty())
			safeId = "report";

		const std::filesystem::path target = PendingDirectory() / (safeId + ".json");

		if (PostToDiscord(webhookUrl, reportJson, errorOut))
		{
			if (EnsureDirectory(SentDirectory()))
				WriteTextFile(SentDirectory() / (safeId + ".json"), reportJson);

			return true;
		}

		if (EnsureDirectory(PendingDirectory()))
		{
			if (WriteTextFile(target, reportJson))
				WriteLastError("queued " + safeId + " for retry: " + (errorOut ? *errorOut : std::string()));
		}

		return false;
	}

	FlushResult FlushPending(const std::string& webhookUrlOverride)
	{
		FlushResult result{};

		std::error_code ec;
		if (!std::filesystem::exists(PendingDirectory(), ec))
			return result;

		std::vector<std::filesystem::path> queued;
		for (const auto& entry : std::filesystem::directory_iterator(PendingDirectory(), ec))
		{
			if (ec)
				break;

			if (entry.is_regular_file() && entry.path().extension() == ".json")
				queued.push_back(entry.path());
		}

		// Oldest first, so reports arrive in the order they happened.
		std::sort(queued.begin(), queued.end());

		EnsureDirectory(SentDirectory());
		EnsureDirectory(FailedDirectory());

		int attempts = 0;
		for (const auto& path : queued)
		{
			if (attempts >= MAX_UPLOADS_PER_FLUSH)
			{
				++result.remaining;
				continue;
			}

			std::string contents;
			if (!ReadTextFile(path, contents))
			{
				++result.remaining;
				continue;
			}

			std::string endpoint = webhookUrlOverride;
			if (endpoint.empty())
			{
				std::string fromReport;
				std::string copy = contents;
				if (ExtractEndpoint(copy, fromReport))
					endpoint = fromReport;
			}

			if (endpoint.empty())
				endpoint = WebhookUrl();

			if (endpoint.empty())
			{
				result.lastError = "no webhook URL available to send the queued reports";
				++result.remaining;
				continue;
			}

			++attempts;

			std::string error;
			if (PostToDiscord(endpoint, contents, &error))
			{
				++result.sent;
				std::error_code moveEc;
				std::filesystem::rename(path, SentDirectory() / path.filename(), moveEc);
				if (moveEc)
					std::filesystem::remove(path, moveEc);
			}
			else
			{
				result.lastError = error;

				// A rejected endpoint will never accept it, so stop retrying and keep it locally.
				if (error.find("rejected the report") != std::string::npos ||
					error.find("must start with") != std::string::npos)
				{
					++result.failed;
					std::error_code moveEc;
					std::filesystem::rename(path, FailedDirectory() / path.filename(), moveEc);
					if (moveEc)
						std::filesystem::remove(path, moveEc);
				}
				else
				{
					++result.remaining;
				}
			}
		}

		if (!result.lastError.empty())
			WriteLastError(result.lastError);

		return result;
	}

	int CountPending()
	{
		std::error_code ec;
		if (!std::filesystem::exists(PendingDirectory(), ec))
			return 0;

		int count = 0;
		for (const auto& entry : std::filesystem::directory_iterator(PendingDirectory(), ec))
		{
			if (ec)
				break;

			if (entry.is_regular_file() && entry.path().extension() == ".json")
				++count;
		}

		return count;
	}

	int DeletePending()
	{
		std::error_code ec;
		if (!std::filesystem::exists(PendingDirectory(), ec))
			return 0;

		int removed = 0;
		for (const auto& entry : std::filesystem::directory_iterator(PendingDirectory(), ec))
		{
			if (ec)
				break;

			if (!entry.is_regular_file())
				continue;

			std::error_code removeEc;
			if (std::filesystem::remove(entry.path(), removeEc) && !removeEc)
				++removed;
		}

		return removed;
	}

	void FlushPendingInBackground()
	{
		std::thread([]()
		{
			// Nothing to do is the common case; don't even spawn the work.
			if (CountPending() == 0)
				return;

			const FlushResult result = FlushPending({});
			if (result.sent > 0)
				std::cout << "Crash reporter: sent " << result.sent << " queued report(s)." << std::endl;
			if (result.remaining > 0 && !result.lastError.empty())
				std::cerr << "Crash reporter: " << result.remaining
					<< " report(s) still queued: " << result.lastError << std::endl;
		}).detach();
	}

	std::string WebhookUrl()
	{
		// Fixed by design: nothing the user can edit (config.yml included) changes this.
		return DISCORD_WEBHOOK_URL;
	}

	int RunSenderMode()
	{
		const FlushResult result = FlushPending({});

		if (result.sent > 0)
			std::cout << "Crash reporter: sent " << result.sent << " report(s)." << std::endl;

		if (result.remaining > 0)
		{
			std::cerr << "Crash reporter: " << result.remaining << " report(s) still queued";
			if (!result.lastError.empty())
				std::cerr << " (" << result.lastError << ")";
			std::cerr << std::endl;
		}

		return result.remaining == 0 ? 0 : 1;
	}

	// -------------------------------------------------------------------------------------------
	// Handlers
	// -------------------------------------------------------------------------------------------
	namespace
	{
		// Spawns "<Comet.exe> --send-crash-reports" so the upload happens in a healthy process
		// rather than inside the one that is already dying.
		void SpawnSenderProcess()
		{
	#if defined(_WIN32)
			const std::wstring exePath = ExecutablePathW();
			if (exePath.empty())
				return;

			std::wstring commandLine = L"\"" + exePath + L"\" --send-crash-reports";

			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};

			// CREATE_NO_WINDOW keeps the helper from flashing a console on screen.
			const BOOL created = CreateProcessW(exePath.c_str(), commandLine.data(),
				nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);

			if (!created)
				return;

			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
	#else
			// Nothing here is safe to run from a signal handler on POSIX, so the report simply waits
			// in the queue and goes out on the next launch instead.
		#endif
		}

		// Writes the report and asks a fresh copy of Comet to upload it. Deliberately does no
		// network work: the process is already in a bad state, and the one place Comet is known to
		// fall over is inside libcurl, so the upload must not share this process.
		void ReportAndTerminate(const std::string& kind, const Fault& fault,
			const std::vector<std::string>& stackTrace, const std::string& note)
		{
			// Re-entrancy guard: if we crash while reporting, just die.
			bool expected = false;
			if (!g_reporting.compare_exchange_strong(expected, true))
				return;

			if (!IsEnabled())
				return;

			try
			{
				const std::string report = BuildReportInternal(kind, fault, stackTrace, note);

				const json parsed = json::parse(report, nullptr, false);
				const std::string reportId = parsed.is_object()
					? parsed.value("reportId", std::string("crash"))
					: std::string("crash");

				std::string safeId;
				for (char c : reportId)
				{
					if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
						c == '-' || c == '_')
						safeId += c;
				}

				if (safeId.empty())
					safeId = "crash";

				const std::filesystem::path directory = PendingDirectory();

				bool written = false;
				if (EnsureDirectory(directory))
					written = WriteTextFile(directory / (safeId + ".json"), report);

				// Console output goes through the tee, so this also lands in the log tail of any
				// *next* report, which is a useful breadcrumb.
				if (written)
					std::cout << "Crash reporter: wrote " << safeId << ".json" << std::endl;
				else
					std::cerr << "Crash reporter: could not write a report to "
						<< directory.string() << std::endl;

				if (written)
					SpawnSenderProcess();
			}
			catch (...)
			{
				// Reporting is best-effort; never let it mask the original failure.
			}
		}

#if defined(_WIN32)
		Fault DescribeSehException(EXCEPTION_POINTERS* info)
		{
			Fault fault{};
			fault.hasFault = true;
			fault.type = "seh";
			fault.threadId = std::to_string(GetCurrentThreadId());

			const DWORD code = info->ExceptionRecord->ExceptionCode;
			fault.code = Hex(code, 8);

			ExceptionDescription description{};
			if (DescribeException(code, description))
			{
				fault.name = description.name;
				fault.description = description.description;
				if (description.hint)
					fault.hint = description.hint;
			}
			else
			{
				fault.name = "UNKNOWN_EXCEPTION";
				fault.description = "Windows did not recognise this exception code.";
			}

			const uint64_t address = reinterpret_cast<uint64_t>(info->ExceptionRecord->ExceptionAddress);
			fault.address = Hex(address, 16);
			fault.module = ModuleForAddress(address, nullptr);

			if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2)
			{
				const ULONG_PTR* parameters = info->ExceptionRecord->ExceptionInformation;
				switch (parameters[0])
				{
					case 0: fault.accessType = "read"; break;
					case 1: fault.accessType = "write"; break;
					case 8: fault.accessType = "execute"; break;
					default: fault.accessType = "unknown"; break;
				}

				fault.accessAddress = Hex(static_cast<uint64_t>(parameters[1]), 16);

				if (parameters[1] == 0)
					fault.detail = "The access address was null, so something dereferenced a null pointer.";
				else if (parameters[1] < 65536)
					fault.detail = "The access address was very close to null, which usually means a "
						"null pointer plus an offset (a member access on a null object).";
			}

			return fault;
		}

		LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
		{
			Fault fault{};
			std::vector<std::string> stackTrace;

			if (info && info->ExceptionRecord)
				fault = DescribeSehException(info);

			// The real stack, starting at the faulting instruction; fall back if it can't be walked.
			stackTrace = CaptureStackTraceFromException(info, MAX_STACK_FRAMES);
			if (stackTrace.empty())
				stackTrace = CaptureStackTrace(MAX_STACK_FRAMES);

			ReportAndTerminate(KIND_CRASH, fault, stackTrace, {});

			// Let Windows Error Reporting and the previous filter see it as well.
			if (g_previousFilter && g_previousFilter != &OnUnhandledException)
				return g_previousFilter(info);

			return EXCEPTION_EXECUTE_HANDLER;
		}
#endif

		void OnTerminate()
		{
			Fault fault{};
			fault.hasFault = true;
			fault.type = "terminate";
			fault.threadId = std::to_string(GetCurrentThreadId());
			fault.name = "ABNORMAL_TERMINATION";
			fault.description = "std::terminate was called, which means an exception escaped to the top "
				"of the stack (or a noexcept guarantee was broken).";

			// Rethrowing lets us name the exception the program died on; if there is none, this
			// is a no-op.
			try
			{
				const std::exception_ptr pending = std::current_exception();
				if (pending)
					std::rethrow_exception(pending);
			}
			catch (const std::exception& e)
			{
				fault.detail = std::string("Uncaught exception: ") + e.what();
				fault.code = Hex(0xE06D7363, 8);
				fault.name = "UNCAUGHT_CPP_EXCEPTION";
			}
			catch (...)
			{
				fault.detail = "Uncaught exception of a non-std::exception type.";
				fault.code = Hex(0xE06D7363, 8);
				fault.name = "UNCAUGHT_CPP_EXCEPTION";
			}

			const std::vector<std::string> stackTrace = CaptureStackTrace(MAX_STACK_FRAMES);
			ReportAndTerminate(KIND_CRASH, fault, stackTrace, {});

			if (g_previousTerminate)
				g_previousTerminate();

			std::abort();
		}

		void OnSignal(int signalNumber)
		{
			Fault fault{};
			fault.hasFault = true;
			fault.type = "signal";
			fault.threadId = std::to_string(GetCurrentThreadId());
			fault.code = std::to_string(signalNumber);

			switch (signalNumber)
			{
				case SIGABRT:
					fault.name = "SIGABRT";
					fault.description = "The program aborted, usually because of a failed assertion or a call to std::abort.";
					break;
				case SIGSEGV:
					fault.name = "SIGSEGV";
					fault.description = "The program made an invalid memory access.";
					break;
				case SIGFPE:
					fault.name = "SIGFPE";
					fault.description = "The program performed an invalid arithmetic operation.";
					break;
				case SIGILL:
					fault.name = "SIGILL";
					fault.description = "The program tried to run an illegal instruction.";
					break;
				default:
					fault.name = "SIGNAL_" + std::to_string(signalNumber);
					fault.description = "An unexpected signal terminated the program.";
					break;
			}

			std::vector<std::string> stackTrace;
			try
			{
				stackTrace = CaptureStackTrace(MAX_STACK_FRAMES);
			}
			catch (...)
			{
			}

			ReportAndTerminate(KIND_CRASH, fault, stackTrace, {});

			// Re-raise with the default handler so the exit code stays meaningful.
			std::signal(signalNumber, SIG_DFL);
			std::raise(signalNumber);
		}

		void OpenSymbols()
		{
		#if defined(_WIN32)
			const HANDLE process = GetCurrentProcess();

			// Undecorated names, load .pdb lazily, resolve file/line, and never pop a dialog about
			// a missing symbol file (that would hang a crash handler forever).
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
				SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);

			// Search next to the executable first so a shipped/CI build finds its own .pdb.
			std::error_code ec;
			const std::filesystem::path exeDir =
				std::filesystem::path(ExecutablePathW()).parent_path();
			SymSetSearchPathW(process, exeDir.wstring().c_str());

			g_symbolsReady = SymInitializeW(process, exeDir.wstring().c_str(), TRUE) != FALSE;
		#endif
		}
	}

	void Install()
	{
		if (g_installed)
			return;

		g_installed = true;
		g_startTime = std::chrono::steady_clock::now();

		OpenSymbols();

	#if defined(_WIN32)
		g_previousFilter = SetUnhandledExceptionFilter(&OnUnhandledException);
		g_previousTerminate = std::set_terminate(&OnTerminate);

		// Only SIGABRT is hooked here. The CRT already wraps main in a __try that maps an access
		// violation to SIGSEGV and calls the SIGSEGV handler first, so installing one would beat
		// the filter above to the punch and discard the exception code, faulting module and access
		// address - the most useful parts of the report. Leaving SIGSEGV/SIGFPE/SIGILL at SIG_DFL
		// lets the CRT's filter pass the exception along, and the SEH handler below sees it whole.
		std::signal(SIGABRT, &OnSignal);

		_set_purecall_handler([]()
		{
			Fault fault{};
			fault.hasFault = true;
			fault.type = "purecall";
			fault.name = "PURE_VIRTUAL_CALL";
			fault.description = "A pure virtual function was called, which usually means a base-class "
				"constructor or destructor reached an override that does not exist yet.";
			fault.threadId = std::to_string(GetCurrentThreadId());
			ReportAndTerminate(KIND_CRASH, fault, CaptureStackTrace(MAX_STACK_FRAMES), {});
		});

		_set_invalid_parameter_handler([](const wchar_t* expression, const wchar_t* function,
			const wchar_t* file, unsigned int line, uintptr_t)
		{
			Fault fault{};
			fault.hasFault = true;
			fault.type = "invalid-parameter";
			fault.name = "INVALID_PARAMETER";
			fault.description = "The C runtime was handed an invalid argument.";
			fault.threadId = std::to_string(GetCurrentThreadId());

			if (function)
				fault.detail = "Function: " + Utf8FromWide(function);
			if (file)
			{
				fault.detail += "\nFile: " + Utf8FromWide(file) + ":" + std::to_string(line);
			}
			if (expression)
				fault.detail += "\nExpression: " + Utf8FromWide(expression);

			ReportAndTerminate(KIND_CRASH, fault, CaptureStackTrace(MAX_STACK_FRAMES), {});
		});
	#else
		// Without SEH the signals are the only way to catch a fault, so all of them are hooked.
		g_previousTerminate = std::set_terminate(&OnTerminate);
		std::signal(SIGABRT, &OnSignal);
		std::signal(SIGSEGV, &OnSignal);
		std::signal(SIGFPE, &OnSignal);
		std::signal(SIGILL, &OnSignal);
	#endif
	}

	void CrashForTesting()
	{
		// A plain null dereference, so the test exercises the same SEH path a real bug would.
		volatile int* doomed = nullptr;
		*doomed = 0x5EED;
	}
}
