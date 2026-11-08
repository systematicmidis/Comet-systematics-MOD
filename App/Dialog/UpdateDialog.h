#pragma once

#include "App/UI/Dialog.h"
#include "UpdateChecker.h"
#include "UpdateBuilder.h"

#include <memory>

// Help > Check for Updates: pick which branch to check, then either open it on GitHub or build it
// here and install the result.
class UpdateDialog : public Dialog
{
public:
	explicit UpdateDialog(UpdateChecker* checker) : Dialog("updateDialog"), checker(checker)
	{
		builder = UpdateBuilder::Create();
	}

	const char* GetTitle() override { return "Check for Updates"; }
	void DrawContent() override;
	void OnOpen() override;

private:
	void Check();
	void DrawBuildSection();

	UpdateChecker* checker = nullptr;
	UpdateChecker::Source selectedSource = UpdateChecker::Source::Official;
	UpdateChecker::UpdateCheckResult result{};
	bool hasResult = false;
	std::shared_ptr<UpdateBuilder> builder;

	// Which build to install and how to get it; both are chosen in the dialog.
	bool debugBuild = false;
	bool downloadExe = false;
};
