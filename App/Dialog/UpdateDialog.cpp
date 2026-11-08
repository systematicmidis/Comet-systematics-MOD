#include "UpdateDialog.h"
#include "Utils.h"

#include <filesystem>

void UpdateDialog::OnOpen()
{
	// A result belongs to the branch that was checked, so don't show it for a fresh visit.
	hasResult = false;
	result = {};

	// A debug build is recognisable by its file name, so the running exe decides the default.
	debugBuild = UpdateBuilder::IsDebugBuild(UpdateBuilder::GetRunningExePath());
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
	ImGui::TextDisabled("Rewrites the Comet you are running from the chosen branch: either compiling it "
		"here with the Visual Studio C++ tools on this PC, or taking the exe attached to the newest "
		"release. Nothing else is kept.");

	auto start = [this]()
	{
		UpdateBuilder::Options options;
		options.source = selectedSource;
		options.configuration = debugBuild ? UpdateBuilder::Configuration::Debug
			: UpdateBuilder::Configuration::Release;
		options.downloadExe = downloadExe;
		builder->Start(options);
	};

	ImGui::Spacing();
	ImGui::BeginDisabled(builder->IsRunning());

	ImGui::Text("Build");
	ImGui::SameLine(150.0f);
	if (ImGui::RadioButton("Release##updateConfig", !debugBuild))
		debugBuild = false;
	ImGui::SameLine();
	if (ImGui::RadioButton("Debug##updateConfig", debugBuild))
		debugBuild = true;
	ImGui::SetItemTooltip("Release is the normal build. Debug keeps Comet's assertions and symbols: "
		"slower, but it reports problems. They are installed as Comet-Release.exe and Comet-Debug.exe, "
		"so the file name says which one it is.");

	ImGui::Text("Get it");
	ImGui::SameLine(150.0f);
	if (ImGui::RadioButton("By building the source##updateMethod", !downloadExe))
		downloadExe = false;
	ImGui::SameLine();
	if (ImGui::RadioButton("From the release's exe##updateMethod", downloadExe))
		downloadExe = true;
	ImGui::SetItemTooltip("Building compiles the branch on this PC, which takes several minutes and "
		"needs Git and the Visual Studio C++ tools. Downloading fetches the prebuilt Comet-Release.exe "
		"or Comet-Debug.exe attached to the newest GitHub release.");

	ImGui::EndDisabled();
	ImGui::Spacing();

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
			if (ImGui::Button(downloadExe ? "Download && Install" : "Build && Install", ImVec2(170, 0)))
				start();
			ImGui::SameLine();
			ImGui::TextDisabled(downloadExe ? "a few seconds" : "takes several minutes");
			break;

		case UpdateBuilder::Stage::Done:
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", status.message.c_str());
			ImGui::TextDisabled("The previous exe is removed the next time Comet starts.");
			drawLogButton();
			break;

		case UpdateBuilder::Stage::Failed:
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "The update failed.");
			if (!status.error.empty())
			{
				ImGui::Spacing();
				ImGui::TextWrapped("%s", status.error.c_str());
			}

			ImGui::Spacing();
			drawLogButton();
			ImGui::SameLine();
			if (ImGui::Button("Try Again", ImVec2(120, 0)))
				start();
			break;

		case UpdateBuilder::Stage::Cancelled:
			ImGui::TextDisabled("%s", status.message.c_str());
			if (ImGui::Button("Start Again", ImVec2(140, 0)))
				start();
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

		// Shown either way: the release's prebuilt exe or a build of this branch is useful even
		// when this copy is already the newest version.
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
