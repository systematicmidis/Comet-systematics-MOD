#pragma once

#include "App/UI/Dialog.h"
#include "IO/ArchiveLoader.h"
#include <string>
#include <vector>

class MainWindow;

// A picker for archives that contain more than one named MIDI. The automatic loader still picks
// the largest track when the archive has only one candidate; this dialog only appears when the
// choice would otherwise be ambiguous.
class ArchiveEntryDialog : public Dialog
{
public:
	ArchiveEntryDialog(MainWindow* window) : Dialog("archive_entry_dialog"), window(window) {}

	const char* GetTitle() override { return "Choose a MIDI in the archive"; }
	void DrawContent() override;
	ImVec2 GetInitialSize() override { return ImVec2(620.0f, 430.0f); }
	ImGuiWindowFlags GetWindowFlags() override
	{
		return ImGuiWindowFlags_NoSavedSettings;
	}

	void Open(const std::string& archivePath, const std::vector<ArchiveLoader::EntryInfo>& entries)
	{
		this->archivePath = archivePath;
		this->entries = entries;
		selected = 0;
		openRequest = true;
	}

private:
	MainWindow* window = nullptr;
	std::string archivePath;
	std::vector<ArchiveLoader::EntryInfo> entries;
	int selected = 0;
};
