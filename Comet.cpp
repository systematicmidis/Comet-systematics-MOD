#include "Comet.h"
#include "App/MainWindow.h"
#include "Telemetry/CometLog.h"
#include "Telemetry/CrashReporter.h"
#include "UpdateBuilder.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <curl/curl.h>
#include <filesystem>
#include "Utils.h"

#ifndef COMET_BUILD_CONFIG
#define COMET_BUILD_CONFIG "unknown"
#endif

namespace
{
	bool HasArgument(int argc, char** argv, const char* wanted)
	{
		for (int i = 1; i < argc; ++i)
		{
			if (std::strcmp(argv[i], wanted) == 0)
				return true;
		}

		return false;
	}
}

int main(int argc, char** argv)
{
	// Capture Comet's own console output from the very start so a crash report carries context.
	CometLog::Install();

	// find where Comet is and set the current dir to that so it can find the assets even if
	// it was launched from somewhere else
	std::filesystem::path exeDir = Utils::GetExecutableDirectory();
	if (!exeDir.empty())
	{
		std::error_code ec;
		std::filesystem::current_path(exeDir, ec);
		if (ec) std::cerr << "Failed to set working directory: " << ec.message() << std::endl;
	}

	if (HasArgument(argc, argv, "--safe-mode"))
	{
#if defined(_WIN32)
		_putenv_s("COMET_SAFE_MODE", "1");
#else
		setenv("COMET_SAFE_MODE", "1", 1);
#endif
	}

	// A path argument opens that MIDI once the window exists, which is also how the release build
	// gets exercised without a file dialog.
	std::string startupMIDI;
	for (int i = 1; i < argc; ++i)
	{
		if (argv[i] == nullptr || argv[i][0] == '-') continue;
		startupMIDI = argv[i];
		break;
	}

	// An update installed earlier left the previous exe behind; this is the first moment it is
	// no longer running.
	UpdateBuilder::RemoveStaleInstall();

	CrashReporter::Install();

#ifdef _DEBUG
	const char* buildType = "Debug";
#else
	const char* buildType = "Release";
#endif

	CrashReporter::SetAppInfo(COMET_VERSION, buildType, COMET_BUILD_CONFIG);

	CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (curlResult != CURLE_OK)
	{
		std::cerr << "Failed to initialize libcurl: "
			<< curl_easy_strerror(curlResult) << std::endl;

		CometLog::Uninstall();
		return 1;
	}

	curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);

	std::cout << "libcurl: " << info->version << '\n';
	std::cout << "SSL: "
		<< (info->ssl_version ? info->ssl_version : "none")
		<< '\n';

	std::string cometVersion = COMET_VERSION;
	std::string versionSuffix = "";
	
	if (!cometVersion.empty() && cometVersion.back() == 'b')
	{
		cometVersion.pop_back();
		versionSuffix = "-Beta";
	}

	MainWindow mainWindow((std::string("Comet v")+cometVersion+versionSuffix).c_str());

	if (!startupMIDI.empty())
		mainWindow.LoadMIDIFile(startupMIDI, std::string());

	mainWindow.Run();

	curl_global_cleanup();
	CometLog::Uninstall();

	return 0;
}
