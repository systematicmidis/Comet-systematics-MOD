#pragma once

#include "App/UI/Dialog.h"
#include "UpdateChecker.h"

// Help > Check for Updates: pick which branch to check, then show what came back.
class UpdateDialog : public Dialog
{
public:
	explicit UpdateDialog(UpdateChecker* checker) : Dialog("updateDialog"), checker(checker) {}

	const char* GetTitle() override { return "Check for Updates"; }
	void DrawContent() override;
	void OnOpen() override;

private:
	void Check();

	UpdateChecker* checker = nullptr;
	UpdateChecker::Source selectedSource = UpdateChecker::Source::Official;
	UpdateChecker::UpdateCheckResult result{};
	bool hasResult = false;
};
