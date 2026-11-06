#include "UpdateDialog.h"
#include "Utils.h"

#include <filesystem>

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

void UpdateDialog::DrawBuildSection()
{
	const UpdateBuilder::Status status = builder->GetStatus();

	ImGui::Spacing();
	ImGui::TextDisabled("Builds this branch from source with the Visual Studio C++ tools on this "
		"PC, then replaces Comet.exe with the result. Nothing else is kept.");

	auto drawLogButton = [&status]()
	{
		const bool hasLog = !status.logPath.empty() && Utils::FileExists(status.logPath);
		ImGui::BeginDisabled(!hasLog);
		if (ImGui::Button("Open Log", ImVec2(120, 0)))
			Utils::OpenFolder(std::filesystem::path(status.logPath).parent_path().string());
		ImGui::EndDisabled();
	};

	switch (status.stage)
	{
		case UpdateBuilder::Stage::Idle:
			if (ImGui::Button("Build && Install", ImVec2(160, 0)))
				builder->Start(selectedSource);
			ImGui::SameLine();
			ImGui::TextDisabled("takes several minutes");
			break;

		case UpdateBuilder::Stage::Done:
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", status.message.c_str());
			ImGui::TextDisabled("The previous exe is removed the next time Comet starts.");
			drawLogButton();
			break;

		case UpdateBuilder::Stage::Failed:
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "The build failed.");
			if (!status.error.empty())
			{
				ImGui::Spacing();
				ImGui::TextWrapped("%s", status.error.c_str());
			}

			ImGui::Spacing();
			drawLogButton();
			ImGui::SameLine();
			if (ImGui::Button("Try Again", ImVec2(120, 0)))
				builder->Start(selectedSource);
			break;

		case UpdateBuilder::Stage::Cancelled:
			ImGui::TextDisabled("%s", status.message.c_str());
			if (ImGui::Button("Start Again", ImVec2(140, 0)))
				builder->Start(selectedSource);
			break;

		default:
			// A stage is running: Resolving, Downloading, Extracting, Configuring, Building or
			// Installing.
			if (status.progress >= 0.0f)
				ImGui::ProgressBar(status.progress, ImVec2(-1, 0));
			else
				ImGui::TextUnformatted("Working...");

			ImGui::TextWrapped("%s", status.message.c_str());
			ImGui::TextDisabled("%s - %.0f s", UpdateBuilder::StageName(status.stage), status.elapsed);
			ImGui::TextDisabled("You can close this window; the build keeps running.");

			if (ImGui::Button("Cancel", ImVec2(120, 0)))
				builder->Cancel();
			break;
	}
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

		if (result.status == UpdateChecker::UPDATE_AVAILABLE)
			DrawBuildSection();
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
