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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace CrashReporter
{
	namespace
	{
		constexpr int MAX_STACK_FRAMES = 64;
		constexpr int LOG_LINES_IN_REPORT = 200;

		// ---------------------------------------------------------------------------------------
		// App-supplied context. Guarded by a mutex, but the crash handler uses try_lock: if another
		// thread died holding it, the log simply goes out with fewer fields.
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
		bool g_enabled = false;

		bool g_installed = false;
		std::chrono::steady_clock::time_point g_startTime;
		// Set before any log is written; a second fatal error while reporting just dies.
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

		std::string ProcessIdText()
		{
#if defined(_WIN32)
			return std::to_string(GetCurrentProcessId());
#else
			return std::to_string(static_cast<long long>(getpid()));
#endif
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
					out = { "STATUS_DLL_NOT_FOUND", "A DLL required to start or continue could not be found.", "A dependency (bass.dll, bassmidi.dll, the MSVC runtime) is missing next to Comet.exe." };
					return true;
				case 0xC0000139:
					out = { "STATUS_ENTRYPOINT_NOT_FOUND", "A DLL was found but does not export a procedure that was called.", "A stale or mixed-version DLL is next to the executable." };
					return true;
				case 0xC0000142:
					out = { "STATUS_DLL_INIT_FAILED", "A DLL failed to initialise.", "Often a missing runtime or a driver DLL refusing to load." };
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

		// One frame as a log line: index, address, owning module, then the symbol and source
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

		// Walks the faulting thread's stack from the exception's own instruction pointer.
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
		// Where logs live
		// ---------------------------------------------------------------------------------------
#if defined(_WIN32)
		std::filesystem::path LogDirectoryPath()
		{
			std::wstring overrideDir;
			if (ReadEnvWide(L"COMET_CRASH_LOG_DIR", overrideDir) && !overrideDir.empty())
				return std::filesystem::path(overrideDir);

			const std::filesystem::path exeDir =
				std::filesystem::path(ExecutablePathW()).parent_path();
			return exeDir / L"logs";
		}
#else
		std::filesystem::path LogDirectoryPath()
		{
			if (const char* overrideDir = std::getenv("COMET_CRASH_LOG_DIR"))
			{
				if (*overrideDir != '\0')
					return std::filesystem::path(overrideDir);
			}

			return std::filesystem::current_path() / "logs";
		}
#endif

		bool EnsureDirectory(const std::filesystem::path& dir)
		{
			std::error_code ec;
			if (std::filesystem::exists(dir, ec))
				return true;

			std::filesystem::create_directories(dir, ec);
			return !ec;
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

		// ---------------------------------------------------------------------------------------
		// Human-readable report
		// ---------------------------------------------------------------------------------------
		struct Fault
		{
			std::string type;        // "seh", "terminate", "signal", "purecall", "invalid-parameter"
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

		std::string SystemText()
		{
			std::ostringstream out;
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
					rtlGetVersion(&version);
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

			out << "OS: " << product << " " << version.major << "." << version.minor << "."
				<< version.build << "\n";

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

			out << "Architecture: " << architecture << " (" << (sizeof(void*) * 8) << "-bit)\n";
			out << "Logical cores: " << sysInfo.dwNumberOfProcessors << "\n";

			// Processor brand string. Leaves 0x80000002..0x80000004 are exactly three 16-byte
			// chunks; the buffer size is derived from the leaf count so the copy can never run
			// past the end, whatever that count is.
			constexpr int BRAND_LEAVES = 3;
			constexpr int BRAND_BYTES = BRAND_LEAVES * 16;

			int registers[4] = {};
			char brand[BRAND_BYTES + 1] = {};

			for (int leaf = 0; leaf < BRAND_LEAVES; ++leaf)
			{
				__cpuid(registers, 0x80000002 + leaf);
				std::memcpy(brand + leaf * 16, registers, sizeof(registers));
			}

			brand[BRAND_BYTES] = '\0';

			std::string brandText = brand;
			while (!brandText.empty() && brandText.front() == ' ')
				brandText.erase(brandText.begin());
			while (!brandText.empty() && brandText.back() == ' ')
				brandText.pop_back();

			out << "CPU: " << (brandText.empty() ? "(unknown)" : brandText) << "\n";

			MEMORYSTATUSEX memory{};
			memory.dwLength = sizeof(memory);
			if (GlobalMemoryStatusEx(&memory))
			{
				out << "Memory: " << (memory.ullTotalPhys / (1024ull * 1024ull)) << " MB total, "
					<< (memory.ullAvailPhys / (1024ull * 1024ull)) << " MB available, "
					<< memory.dwMemoryLoad << "% load\n";
			}

			out << "Screen: " << GetSystemMetrics(SM_CXSCREEN) << "x"
				<< GetSystemMetrics(SM_CYSCREEN) << "\n";

			wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
			if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
				out << "Locale: " << Utf8FromWide(locale) << "\n";

			TIME_ZONE_INFORMATION zone{};
			if (GetTimeZoneInformation(&zone) != TIME_ZONE_ID_INVALID)
				out << "Timezone: " << Utf8FromWide(zone.StandardName) << "\n";

			out << "GPU vendor: " << (g_gpuVendor.empty() ? "(unknown)" : g_gpuVendor) << "\n";
			out << "GPU renderer: " << (g_gpuRenderer.empty() ? "(unknown)" : g_gpuRenderer) << "\n";
			out << "GPU driver: " << (g_gpuVersion.empty() ? "(unknown)" : g_gpuVersion) << "\n";
			out << "OpenGL: " << (g_glVersion.empty() ? "(unknown)" : g_glVersion) << "\n";
#else
			out << "OS: Non-Windows\n";
			out << "Architecture: " << (sizeof(void*) * 8) << "-bit\n";
			out << "Logical cores: " << std::thread::hardware_concurrency() << "\n";
#endif
			return out.str();
		}

		std::string ApplicationText()
		{
			std::ostringstream out;
			out << "Version: " << g_version << "\n";
			out << "Build type: " << g_buildType << "\n";
			out << "Build config: " << (g_buildConfig.empty() ? "(unknown)" : g_buildConfig) << "\n";

#if defined(_MSC_VER)
			out << "Compiler: MSVC " << _MSC_VER << "\n";
#elif defined(__clang__)
			out << "Compiler: Clang " << __clang_version__ << "\n";
#elif defined(__GNUC__)
			out << "Compiler: GCC " << __VERSION__ << "\n";
#else
			out << "Compiler: Unknown\n";
#endif

			out << "Build date: " << __DATE__ << " " << __TIME__ << "\n";
			return out.str();
		}

		std::string EnvironmentText()
		{
			std::ostringstream out;
			out << "UTC time: " << UtcTimestamp() << "\n";
			out << "Local time: " << LocalTimestamp() << "\n";
			out << "Process ID: " << ProcessIdText() << "\n";
			out << "Process uptime: " << FormatUptime(UptimeSeconds()) << "\n";

#if defined(_WIN32)
			out << "Executable: " << Utf8FromWide(ExecutablePathW()) << "\n";
			out << "Working directory: " << Utf8FromWide(CurrentDirectoryW()) << "\n";
			out << "Current thread: " << GetCurrentThreadId() << "\n";
#else
			out << "Working directory: " << std::filesystem::current_path().string() << "\n";
#endif
			return out.str();
		}

		std::string FaultText(const Fault& fault, const std::string& note)
		{
			std::ostringstream out;

			if (!note.empty())
				out << "Note: " << note << "\n\n";

			if (!fault.hasFault)
			{
				out << "No exception: this log was requested from the app rather than produced by a crash.\n";
				return out.str();
			}

			out << "Type: " << fault.type << "\n";
			if (!fault.code.empty()) out << "Code: " << fault.code << "\n";
			if (!fault.name.empty()) out << "Name: " << fault.name << "\n";
			if (!fault.description.empty()) out << "Description: " << fault.description << "\n";
			if (!fault.hint.empty()) out << "Hint: " << fault.hint << "\n";
			if (!fault.module.empty()) out << "Module: " << fault.module << "\n";
			if (!fault.address.empty()) out << "Address: " << fault.address << "\n";
			if (!fault.threadId.empty()) out << "Thread: " << fault.threadId << "\n";
			if (!fault.accessType.empty()) out << "Access: " << fault.accessType << "\n";
			if (!fault.accessAddress.empty()) out << "Access address: " << fault.accessAddress << "\n";
			if (!fault.detail.empty()) out << "Detail: " << fault.detail << "\n";
			return out.str();
		}

		std::string StateText()
		{
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

			std::ostringstream out;
			out << "Context available: " << (haveContext ? "yes" : "no") << "\n";
			if (haveContext)
			{
				out << "MIDI path: " << (midi.path.empty() ? "(none)" : midi.path) << "\n";
				out << "MIDI name: " << (midi.displayName.empty() ? "(none)" : midi.displayName) << "\n";
				out << "Notes: " << midi.notes << "\n";
				out << "Length ticks: " << midi.lengthTicks << "\n";
				out << "Track count: " << midi.trackCount << "\n";
				out << "Note tracks: " << midi.noteTrackCount << "\n";
				out << "Resolution: " << midi.resolution << "\n";
				out << "Length seconds: " << midi.lengthSeconds << "\n";
				out << "Sequence loaded: " << (flags.hasSequence ? "yes" : "no") << "\n";
				out << "Playing: " << (flags.playing ? "yes" : "no") << "\n";
				out << "Rendering: " << (flags.rendering ? "yes" : "no") << "\n";
				out << "Loading: " << (flags.loading ? "yes" : "no") << "\n";
			}
			return out.str();
		}

		std::string SettingsText()
		{
			std::string snapshot;
			{
				std::unique_lock<std::mutex> lock(g_contextMutex, std::try_to_lock);
				if (lock.owns_lock())
					snapshot = g_settingsSnapshot;
			}

			if (snapshot.empty())
				return "(unavailable)\n";
			return snapshot + "\n";
		}

		std::string BuildLogText(const std::string& kind, const Fault& fault,
			const std::vector<std::string>& stackTrace, const std::string& note)
		{
			std::ostringstream out;

			out << "Comet crash log\n";
			out << "===============\n\n";
			out << "Kind: " << kind << "\n";
			out << "Report ID: " << StampForId() << "-" << ProcessIdText() << "\n\n";

			out << "--- Application ---\n" << ApplicationText() << "\n";
			out << "--- Environment ---\n" << EnvironmentText() << "\n";
			out << "--- System ---\n" << SystemText() << "\n";
			out << "--- Exception ---\n" << FaultText(fault, note) << "\n";
			out << "--- Stack trace ---\n";
			if (stackTrace.empty())
				out << "(no frames captured)\n";
			else
			{
				for (const std::string& frame : stackTrace)
					out << frame << "\n";
			}
			out << "\n--- Loaded MIDI and state ---\n" << StateText() << "\n";
			out << "--- Settings ---\n" << SettingsText() << "\n";
			out << "--- Console output (last " << LOG_LINES_IN_REPORT << " lines) ---\n";
			out << CometLog::Tail(LOG_LINES_IN_REPORT) << "\n";

			out << "Send this file in the Systematics Discord when reporting the crash.\n";
			return out.str();
		}

		std::filesystem::path WriteLogText(const std::string& kind, const std::string& text)
		{
			const std::filesystem::path directory = LogDirectoryPath();
			if (!EnsureDirectory(directory))
				return {};

			std::string safeKind = kind;
			for (char& c : safeKind)
			{
				if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
					c == '-' || c == '_'))
					c = '-';
			}

			const std::filesystem::path path = directory /
				("comet-" + safeKind + "-" + StampForId() + "-" + ProcessIdText() + ".log");

			if (!WriteTextFile(path, text))
				return {};
			return path;
		}

		// ---------------------------------------------------------------------------------------
		// Handlers
		// ---------------------------------------------------------------------------------------
		void ReportAndTerminate(const std::string& kind, const Fault& fault,
			const std::vector<std::string>& stackTrace, const std::string& note)
		{
			// Re-entrancy guard: if we crash while writing the log, just die.
			bool expected = false;
			if (!g_reporting.compare_exchange_strong(expected, true))
				return;

			if (!g_enabled)
				return;

			try
			{
				const std::string text = BuildLogText(kind, fault, stackTrace, note);
				const std::filesystem::path path = WriteLogText(kind, text);

				if (!path.empty())
					std::cout << "Crash reporter: wrote " << path.string() << std::endl;
				else
					std::cerr << "Crash reporter: could not write a crash log" << std::endl;
			}
			catch (...)
			{
				// Logging is best-effort; never let it mask the original failure.
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
		// address - the most useful parts of the log. Leaving SIGSEGV/SIGFPE/SIGILL at SIG_DFL
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
				fault.detail += "\nFile: " + Utf8FromWide(file) + ":" + std::to_string(line);
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
		g_enabled = enabled;
	}

	std::string LogDirectory()
	{
		const std::filesystem::path directory = LogDirectoryPath();
		EnsureDirectory(directory);

#if defined(_WIN32)
		return Utf8FromWide(directory.wstring());
#else
		return directory.string();
#endif
	}

	std::string WriteLog(const std::string& kind, const std::string& note)
	{
		try
		{
			const Fault noFault{};
			const std::string text = BuildLogText(kind, noFault, CaptureStackTrace(MAX_STACK_FRAMES), note);
			const std::filesystem::path path = WriteLogText(kind, text);

#if defined(_WIN32)
			return path.empty() ? std::string() : Utf8FromWide(path.wstring());
#else
			return path.empty() ? std::string() : path.string();
#endif
		}
		catch (...)
		{
			return {};
		}
	}

	void CrashForTesting()
	{
		// A plain null dereference, so the test exercises the same SEH path a real bug would.
		volatile int* doomed = nullptr;
		*doomed = 0x5EED;
	}
}
