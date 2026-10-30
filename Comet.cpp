#include "Comet.h"
#include "App/MainWindow.h"
#include "Telemetry/CometLog.h"
#include "Telemetry/CrashReporter.h"
#include <cstring>
#include <string>
#include <curl/curl.h>

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

	// After a crash Comet relaunches itself with this flag to upload the queued report. That
	// helper must not open a window, load a config or install crash handlers of its own.
	const bool senderMode = HasArgument(argc, argv, "--send-crash-reports");

	if (!senderMode)
	{
		CrashReporter::Install();

#ifdef _DEBUG
		const char* buildType = "Debug";
#else
		const char* buildType = "Release";
#endif

		CrashReporter::SetAppInfo(COMET_VERSION, buildType, COMET_BUILD_CONFIG);
	}

	CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (curlResult != CURLE_OK)
	{
		std::cerr << "Failed to initialize libcurl: "
			<< curl_easy_strerror(curlResult) << std::endl;

		CometLog::Uninstall();
		return 1;
	}

	if (senderMode)
	{
		const int code = CrashReporter::RunSenderMode();
		curl_global_cleanup();
		CometLog::Uninstall();
		return code;
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

	// Retry anything a previous run couldn't deliver.
	CrashReporter::FlushPendingInBackground();

	mainWindow.Run();

	curl_global_cleanup();
	CometLog::Uninstall();

	return 0;
}
