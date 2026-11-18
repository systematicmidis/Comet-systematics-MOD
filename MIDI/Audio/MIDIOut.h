#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32

#include <vector>
#include <Windows.h>

typedef void (*KDMAPI_Init)();
typedef void (*KDMAPI_Terminate)();
typedef void (*KDMAPI_Send)(uint32_t msg);

class MIDIOut
{
public:
	MIDIOut() = default;
	~MIDIOut();
	void Initialize();

	// A driver fault is reported to the UI on the next frame; returns false when there is none.
	static bool ConsumeFaultNotice(std::string& message);

	// Default on: the DLL lives in a small helper process (this executable --midi-host), so a
	// fault in OmniMIDI terminates the helper instead of Comet. Set COMET_MIDI_ISOLATION=0 to
	// keep the old in-process path for troubleshooting.
	static void SetIsolationEnabled(bool enabled);
	static bool IsIsolationEnabled();
	static int RunHostMode();

	void Shutdown();
	void SendEvent(uint32_t msg);
private:
	void InitializeInProcess(bool interactive = true);
	void ShutdownInProcess();
	void SendInProcess(uint32_t msg);

	bool StartIsolated();
	void ShutdownIsolated();
	bool SendIsolated(uint32_t msg);

	HMODULE omni = nullptr;

	KDMAPI_Init InitStream = nullptr;
	KDMAPI_Send Send = nullptr;
	KDMAPI_Terminate End = nullptr;

	bool initialized = false;
	bool attempted = false;
	bool isolated = false;
	void* hostProcess = nullptr;
	void* hostInput = nullptr;
};

#else

class MIDIOut
{
public:
	MIDIOut() = default;
	~MIDIOut();
	void Initialize();
	void Shutdown();
	void SendEvent(uint32_t msg);
private:
	void* handle = nullptr;
	bool attempted = false;
	void (*sendDirectData)(uint32_t msg) = nullptr;
	void (*terminateStream)() = nullptr;
};
#endif