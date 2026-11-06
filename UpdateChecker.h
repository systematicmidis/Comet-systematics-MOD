#pragma once

#include <string>
#include <string_view>
#include "Comet.h"

enum VersionStage : int
{
	ALPHA = 0,
	BETA,
	RELEASE
};

struct Version
{
	int major;
	int minor;
	int patch;
	VersionStage stage = RELEASE;

	auto operator<=>(const Version& other) const = default;

	static Version Parse(std::string_view str);
	const std::string ToString() const;
};

class UpdateChecker
{
public:
	enum UpdateStatus
	{
		UP_TO_DATE,
		UPDATE_AVAILABLE,
		CHECK_ERROR
	};

	// The branches offered by Help > Check for Updates.
	enum class Source
	{
		Official = 0,     // MrCHB1/Comet, master
		SystematicsMOD    // systematicmidis/Comet-systematics-MOD, main
	};

	struct SourceInfo
	{
		const char* name;     // label shown in the dialog
		const char* repo;     // owner/name on GitHub
		const char* branch;
		const char* repoUrl;
	};

	struct UpdateCheckResult
	{
		UpdateStatus status = UP_TO_DATE;
		std::string githubUrl;
		// Extra lines for the dialog: the release that is available, or the newest commit seen.
		std::string detail;
	};

	// The exact source tree of the branch as it is right now, for building an update locally.
	struct SourcePackage
	{
		bool ok = false;
		std::string sha;
		std::string zipUrl; // codeload archive of that commit
		std::string error;
	};

	static SourcePackage ResolvePackage(Source source);

	static constexpr int SOURCE_COUNT = 2;
	static const SourceInfo& GetSourceInfo(Source source);

	UpdateChecker() = default;
	UpdateCheckResult CheckForUpdate(Source source);
private:
	Version currVersion = Version::Parse(COMET_VERSION);
};
