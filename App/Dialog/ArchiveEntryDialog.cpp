#include "ArchiveEntryDialog.h"
#include "App/MainWindow.h"
#include <cstdio>

void ArchiveEntryDialog::DrawContent()
{
	ImGui::TextWrapped("%s", archivePath.c_str());
	ImGui::TextDisabled("%d MIDI files found. Pick the one to load.", (int)entries.size());
	ImGui::Spacing();

	ImGui::BeginChild("##archive_entries", ImVec2(0.0f, 300.0f), true);
	for (int i = 0; i < (int)entries.size(); ++i)
	{
		const auto& entry = entries[(size_t)i];
		char label[1024];
		if (entry.size > 0)
			std::snprintf(label, sizeof(label), "%s  (%llu bytes)", entry.name.c_str(),
				(unsigned long long)entry.size);
		else
			std::snprintf(label, sizeof(label), "%s", entry.name.c_str());

		if (ImGui::Selectable(label, selected == i))
			selected = i;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", entry.name.c_str());
	}
	ImGui::EndChild();

	ImGui::Spacing();
	const bool canOpen = !entries.empty() && selected >= 0 && selected < (int)entries.size();
	ImGui::BeginDisabled(!canOpen);
	if (ImGui::Button("Open", ImVec2(120, 0)))
	{
		window->LoadMIDIFile(archivePath, entries[(size_t)selected].name);
		Close();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0)))
		Close();
}
