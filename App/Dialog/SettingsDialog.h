#pragma once

#include "App/MIDIApp.h"
#include "App/UI/Dialog.h"

class SettingsDialog : public Dialog
{
public:
	SettingsDialog(MIDIApp* app) : Dialog("settingsDialog"), app(app) {}
	const char* GetTitle() override { return "Settings"; }
	void DrawContent() override;
	ImVec2 GetInitialSize() override { return ImVec2(760.0f, 650.0f); }
	ImGuiWindowFlags GetWindowFlags() override { return ImGuiWindowFlags_NoSavedSettings; }
private:
	MIDIApp* app;

	void DrawAppTab();
	void DrawVisualTab();
	void DrawAudioTab();
	void DrawMIDITab();
	void DrawMultiplierTab();
	void DrawCrashReportsTab();

	// Pushes the crash-report settings into the reporter (and optionally straight to disk, since a
	// crash means the usual save-on-exit never runs).
	void ApplyCrashConfiguration(bool saveNow);

	// Note counts per track of the loaded MIDI. Counting them walks the whole note data, so the
	// result is cached until a different sequence shows up.
	const std::vector<uint64_t>& TrackNoteTotals(const MIDISequence* sequence);
	const MIDISequence* trackTotalsSequence = nullptr;
	std::vector<uint64_t> trackNoteTotals{};

	// Editable buffer for the custom multiplier field; refreshed from the config while the
	// field isn't focused so preset clicks show up immediately.
	char multiplierInput[512] = "";
	bool multiplierInputActive = false;

	// "Target note count" field: type a count and the multiplier is solved for the loaded MIDI.
	char multiplierTargetInput[512] = "";
	bool multiplierTargetActive = false;

	// The test send runs on a worker so an unreachable webhook can't stall the frame. Shared
	// ownership keeps its state alive even if the reply lands after the dialog is gone.
	struct CrashTestState
	{
		std::atomic_bool inFlight{ false };
		std::atomic_bool succeeded{ false };
		std::mutex mutex;
		std::string message;
	};
	std::shared_ptr<CrashTestState> crashTest = std::make_shared<CrashTestState>();
};