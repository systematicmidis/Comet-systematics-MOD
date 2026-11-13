#include "MIDIOut.h"
#include <iostream>
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

	// Present in both v1 and v2; reporting it makes a wrong DLL obvious in the log.
	if (auto version = (int(*)())GetProcAddress(omni, "ReturnKDMAPIVer"))
		std::cout << "  KDMAPI v" << version() << std::endl;

	InitStream();
	initialized = true;
}

MIDIOut::~MIDIOut()
{
	Shutdown();
}

void MIDIOut::Shutdown()
{
	if (initialized && End)
		End();

	if (omni) FreeLibrary(omni);

	omni = nullptr;
	InitStream = nullptr;
	Send = nullptr;
	End = nullptr;
	initialized = false;
	attempted = false;
}

void MIDIOut::SendEvent(uint32_t msg)
{
	if (initialized && Send) Send(msg);
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