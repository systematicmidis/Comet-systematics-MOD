#include "UpdateDialog.h"
#include "Utils.h"

void UpdateDialog::OnOpen()
{
	// A result belongs to the branch that was checked, so don't show it for a fresh visit.
	hasResult = false;
	result = {};
}

void UpdateDialog::Check()
{
	if (checker == nullptr)
		return;

	result = checker->CheckForUpdate(selectedSource);
	hasResult = true;
}

void UpdateDialog::DrawContent()
{
	ImGui::TextWrapped("Choose which branch of Comet to check for updates.");
	ImGui::Spacing();

	for (int i = 0; i < UpdateChecker::SOURCE_COUNT; i++)
	{
		const UpdateChecker::Source source = (UpdateChecker::Source)i;
		const UpdateChecker::SourceInfo& info = UpdateChecker::GetSourceInfo(source);

		if (ImGui::RadioButton(info.name, selectedSource == source))
		{
			selectedSource = source;
			// The old answer was about a different branch, so don't keep showing it.
			hasResult = false;
		}

		ImGui::SetItemTooltip("%s", info.repoUrl);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (!hasResult)
	{
		ImGui::TextDisabled("Pick a branch and press Check.");
	}
	else
	{
		switch (result.status)
		{
			case UpdateChecker::UPDATE_AVAILABLE:
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "An update is available.");
				break;
			case UpdateChecker::UP_TO_DATE:
				ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "This build is up to date.");
				break;
			case UpdateChecker::CHECK_ERROR:
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Could not check for updates.");
				break;
		}

		if (!result.detail.empty())
		{
			ImGui::Spacing();
			ImGui::TextWrapped("%s", result.detail.c_str());
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::Button(hasResult ? "Check Again" : "Check", ImVec2(120, 0)))
		Check();

	ImGui::SameLine();
	ImGui::BeginDisabled(!hasResult || result.githubUrl.empty());
	if (ImGui::Button("Open GitHub", ImVec2(120, 0)))
		Utils::OpenURL(result.githubUrl);
	ImGui::EndDisabled();

	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(120, 0)))
		Close();
}
