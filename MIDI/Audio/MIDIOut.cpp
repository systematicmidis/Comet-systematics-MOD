#include "MIDIOut.h"
#include <iostream>
#include <mutex>
#include <string>
#include "Utils.h"

#ifdef _WIN32

namespace
{
	// OmniMIDI v2 keeps its DLL in its own folder instead of registering it on PATH, so the
	// install locations are tried after the plain name. This finds v2 and v1 both.
	HMODULE LoadOmniMIDI()
	{
		HMODULE omni = LoadLibraryA("OmniMIDI.dll");
		if (omni) return omni;

		const char* const roots[] = { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" };
		for (const char* root : roots)
		{
			char base[MAX_PATH]{};
			if (GetEnvironmentVariableA(root, base, MAX_PATH) == 0)
				continue;

			const std::string path = std::string(base) + "\\OmniMIDI\\OmniMIDI.dll";
			omni = LoadLibraryA(path.c_str());
			if (omni) return omni;
		}

		return nullptr;
	}

	// A driver can fault inside its own entry points; without this the whole app dies when the
	// installed OmniMIDI is broken or mismatched. These wrappers turn that into "no MIDI out".
	bool CallNoArgs(void (*fn)()) noexcept
	{
		__try
		{
			fn();
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	std::mutex noticeMutex;
	std::string noticeMessage;
	bool noticePending = false;

	void SetFaultNotice(const char* message)
	{
		std::lock_guard<std::mutex> lock(noticeMutex);
		noticeMessage = message;
		noticePending = true;
	}

	// Sends run per note, so this has to stay cheap; x64 SEH costs nothing until something throws.
	bool CallSend(KDMAPI_Send fn, uint32_t msg) noexcept
	{
		__try
		{
			fn(msg);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool QueryVersion(int (*fn)(), int& version) noexcept
	{
		__try
		{
			version = fn();
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}
}

void MIDIOut::Initialize()
{
	if (attempted) return;
	attempted = true;

	std::cout << "Loading MIDI out" << std::endl;

	omni = LoadOmniMIDI();
	if (!omni)
	{
		Utils::Dialogs::ShowDialog(
			"KDMAPI warning",
			"OmniMIDI v2 is not installed, or OmniMIDI.dll could not be found. Comet expects the "
			"driver in its install folder (usually C:\\Program Files\\OmniMIDI). It will continue "
			"without it.",
			Utils::Dialogs::DialogType::DIALOG_WARNING
		);
		return;
	}

	InitStream = (KDMAPI_Init)GetProcAddress(omni, "InitializeKDMAPIStream");
	End = (KDMAPI_Terminate)GetProcAddress(omni, "TerminateKDMAPIStream");

	// v2's unbuffered entry point is the low-latency one; v1 and older v2 builds only export the
	// buffered SendDirectData, so it is the fallback rather than the first choice.
	Send = (KDMAPI_Send)GetProcAddress(omni, "SendDirectDataNoBuf");
	if (!Send) Send = (KDMAPI_Send)GetProcAddress(omni, "SendDirectData");

	if (!InitStream || !Send || !End)
	{
		Utils::Dialogs::ShowDialog(
			"KDMAPI warning",
			"OmniMIDI was found, but one or more required KDMAPI functions could not be loaded.",
			Utils::Dialogs::DialogType::DIALOG_WARNING
		);

		FreeLibrary(omni);
		omni = nullptr;
		InitStream = nullptr;
		Send = nullptr;
		End = nullptr;
		return;
	}

	// Present in both v1 and v2; reporting it makes a wrong DLL obvious in the log. The call is
	// guarded because an incompatible OmniMIDI build can fault here, which used to crash the app
	// right after a MIDI finished loading.
	if (auto version = (int(*)())GetProcAddress(omni, "ReturnKDMAPIVer"))
	{
		int value = 0;
		if (!QueryVersion(version, value))
		{
			std::cerr << "  KDMAPI driver faulted in ReturnKDMAPIVer; continuing without MIDI out." << std::endl;
		SetFaultNotice("OmniMIDI faulted while Comet started MIDI output, so this session will have no "
			"sound. The MIDI finished loading and Comet will keep running without MIDI out.\n\n"
			"Reinstall OmniMIDI (v2) if you need playback through a MIDI device.");
			FreeLibrary(omni);
			omni = nullptr;
			InitStream = nullptr;
			Send = nullptr;
			End = nullptr;
			return;
		}

		std::cout << "  KDMAPI v" << value << std::endl;
	}

	if (!CallNoArgs(InitStream))
	{
		std::cerr << "  KDMAPI driver faulted in InitializeKDMAPIStream; continuing without MIDI out." << std::endl;
		SetFaultNotice("OmniMIDI faulted while Comet started MIDI output, so this session will have no "
			"sound. The MIDI finished loading and Comet will keep running without MIDI out.\n\n"
			"Reinstall OmniMIDI (v2) if you need playback through a MIDI device.");
		FreeLibrary(omni);
		omni = nullptr;
		InitStream = nullptr;
		Send = nullptr;
		End = nullptr;
		return;
	}

	initialized = true;
}

bool MIDIOut::ConsumeFaultNotice(std::string& message)
{
	std::lock_guard<std::mutex> lock(noticeMutex);
	if (!noticePending) return false;

	message = noticeMessage;
	noticePending = false;
	return true;
}

MIDIOut::~MIDIOut()
{
	Shutdown();
}

void MIDIOut::Shutdown()
{
	if (initialized && End)
		CallNoArgs(End);

	// Clear the entry points before unloading: a send racing this would otherwise jump into freed
	// code, which is exactly the kind of fault the guards exist to prevent.
	initialized = false;
	InitStream = nullptr;
	Send = nullptr;
	End = nullptr;

	if (omni) FreeLibrary(omni);
	omni = nullptr;
	attempted = false;
}

void MIDIOut::SendEvent(uint32_t msg)
{
	if (!initialized || !Send) return;

	if (!CallSend(Send, msg))
	{
		std::cerr << "  KDMAPI driver faulted in Send; disabling MIDI out." << std::endl;
		Send = nullptr;
		SetFaultNotice("OmniMIDI faulted while sending MIDI data, so notes stopped reaching the "
			"MIDI device and playback will be silent.\n\n"
			"Reinstall OmniMIDI (v2) if you need playback through a MIDI device.");
	}
}

#else

#include <dlfcn.h>
#include <unistd.h>
#include <climits>
#include <string>

typedef void (*KDMAPI_Init)();
typedef void (*KDMAPI_Terminate)();
typedef void (*KDMAPI_Send)(uint32_t msg);

static void* LoadOmniMIDI()
{
	// try the standard library search path before fallback
	void* handle = dlopen("libOmniMIDI.so", RTLD_NOW | RTLD_LOCAL);
	if (handle)
		return handle;

	// fall back to current directory
	char buffer[PATH_MAX];
	ssize_t count = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
	if (count > 0)
	{
		std::string path(buffer, count);
		size_t slash = path.find_last_of('/');
		if (slash != std::string::npos)
		{
			path = path.substr(0, slash + 1) + "libOmniMIDI.so";
			handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		}
	}
	return handle;
}

void MIDIOut::Initialize()
{
	if (attempted) return;
	attempted = true;

	std::cout << "Loading MIDI out" << std::endl;
	handle = LoadOmniMIDI();
	if (!handle)
	{
		std::cerr << "Failed to load libOmniMIDI.so: " << dlerror() << std::endl;
		return;
	}

	auto init = (KDMAPI_Init)dlsym(handle, "InitializeKDMAPIStream");
	sendDirectData = (KDMAPI_Send)dlsym(handle, "SendDirectDataNoBuf");
	if (!sendDirectData) sendDirectData = (KDMAPI_Send)dlsym(handle, "SendDirectData");
	terminateStream = (KDMAPI_Terminate)dlsym(handle, "TerminateKDMAPIStream");

	if (!init || !sendDirectData || !terminateStream)
	{
		std::cerr << "libOmniMIDI.so is missing KDMAPI symbols" << std::endl;
		dlclose(handle);
		handle = nullptr;
		sendDirectData = nullptr;
		terminateStream = nullptr;
		return;
	}

	init();
}

MIDIOut::~MIDIOut()
{
	Shutdown();
}

void MIDIOut::Shutdown()
{
	if (terminateStream)
		terminateStream();
	if (handle)
		dlclose(handle);

	handle = nullptr;
	sendDirectData = nullptr;
	terminateStream = nullptr;
	attempted = false;
}

void MIDIOut::SendEvent(uint32_t msg)
{
	if (sendDirectData)
		sendDirectData(msg);
}
#endif