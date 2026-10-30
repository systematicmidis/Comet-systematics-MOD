#include "UpdateChecker.h"
#include <string_view>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
	const UpdateChecker::SourceInfo SOURCES[UpdateChecker::SOURCE_COUNT] = {
		{ "Default branch (MrCHB1/Comet - master)", "MrCHB1/Comet", "master", "https://github.com/MrCHB1/Comet" },
		{ "Systematics MOD branch (systematicmidis/Comet-systematics-MOD - main)", "systematicmidis/Comet-systematics-MOD", "main", "https://github.com/systematicmidis/Comet-systematics-MOD" }
	};

	struct HttpResponse
	{
		CURLcode code = CURLE_OK;
		long status = 0;
		std::string body;
	};

	size_t WriteResponse(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		auto* str = static_cast<std::string*>(userdata);
		str->append(ptr, size * nmemb);
		return size * nmemb;
	}

	HttpResponse HttpGet(const std::string& url)
	{
		HttpResponse response;

		CURL* curl = curl_easy_init();
		if (curl == nullptr)
		{
			response.code = CURLE_FAILED_INIT;
			return response;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "Comet");
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

		response.code = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
		curl_easy_cleanup(curl);

		return response;
	}

	// __DATE__ is "Oct 30 2026". Only the day matters when deciding whether a branch has newer
	// commits than this build.
	std::string BuildDate()
	{
		static const char* const monthNames[] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};

		char monthName[4] = {};
		int day = 0;
		int year = 0;
		if (std::sscanf(__DATE__, "%3s %d %d", monthName, &day, &year) != 3)
			return {};

		int month = 0;
		for (int i = 0; i < 12; i++)
		{
			if (std::strcmp(monthName, monthNames[i]) == 0)
			{
				month = i + 1;
				break;
			}
		}

		if (month == 0)
			return {};

		char text[16];
		std::snprintf(text, sizeof(text), "%04d-%02d-%02d", year, month, day);
		return text;
	}

	// First line of the commit message, so the dialog stays readable.
	std::string CommitSummary(const nlohmann::json& commitJson)
	{
		std::string message = commitJson.value("commit", nlohmann::json::object())
			.value("message", std::string());

		const std::size_t newline = message.find_first_of("\r\n");
		if (newline != std::string::npos)
			message.resize(newline);

		return message;
	}
}

Version Version::Parse(std::string_view str)
{
	VersionStage stage = RELEASE;

	if (str.starts_with('v')) str.remove_prefix(1);
	if (str.ends_with('a'))
	{
		stage = ALPHA;
		str.remove_suffix(1);
	}
	if (str.ends_with('b'))
	{
		stage = BETA;
		str.remove_suffix(1);
	}

	Version v{};
	std::sscanf(str.data(), "%d.%d.%d", &v.major, &v.minor, &v.patch);
	v.stage = stage;

	return v;
}

const std::string Version::ToString() const
{
	std::ostringstream result;
	result << "v" << major << "." << minor << "." << patch;
	if (stage == ALPHA) result << "-Alpha";
	if (stage == BETA) result << "-Beta";
	return result.str();
}

const UpdateChecker::SourceInfo& UpdateChecker::GetSourceInfo(Source source)
{
	const int index = (int)source;
	if (index < 0 || index >= SOURCE_COUNT)
		return SOURCES[0];

	return SOURCES[index];
}

UpdateChecker::UpdateCheckResult UpdateChecker::CheckForUpdate(Source source)
{
	const SourceInfo& info = GetSourceInfo(source);
	std::cout << "Update check begin (" << info.repo << " @ " << info.branch << ")." << std::endl;

	try
	{
#pragma region Github commit retrieval
		HttpResponse commitsResponse = HttpGet("https://api.github.com/repos/" + std::string(info.repo) + "/commits/" + info.branch);

		if (commitsResponse.code != CURLE_OK)
		{
			std::cout << "  Curl error: " << curl_easy_strerror(commitsResponse.code) << std::endl;
			return { UpdateStatus::CHECK_ERROR, info.repoUrl, "Could not reach GitHub." };
		}

		if (commitsResponse.status != 200)
		{
			std::cout << "  GitHub returned HTTP " << commitsResponse.status << "." << std::endl;
			return { UpdateStatus::CHECK_ERROR, info.repoUrl,
				"GitHub returned HTTP " + std::to_string(commitsResponse.status) + " for the " + info.branch + " branch." };
		}

		auto commitsJson = nlohmann::json::parse(commitsResponse.body, nullptr, false);
		if (commitsJson.is_discarded())
		{
			std::cout << "  Invalid JSON retrieved." << std::endl;
			return { UpdateStatus::CHECK_ERROR, info.repoUrl, "GitHub sent back something that isn't valid JSON." };
		}

		const std::string sha = commitsJson.value("sha", "");
		if (sha.empty())
		{
			std::cout << "  JSON retrieved but did not have a commit." << std::endl;
			return { UpdateStatus::CHECK_ERROR, info.repoUrl, "GitHub did not return a commit for the " + std::string(info.branch) + " branch." };
		}

		const std::string commitUrl = commitsJson.value("html_url", std::string(info.repoUrl));
		const std::string commitDate = commitsJson.value("commit", nlohmann::json::object())
			.value("author", nlohmann::json::object())
			.value("date", "");

		std::string commitLabel = sha.substr(0, std::min<std::size_t>(sha.size(), 7));
		if (commitDate.size() >= 10)
			commitLabel += " (" + commitDate.substr(0, 10) + ")";

		const std::string summary = CommitSummary(commitsJson);
		const std::string detailSuffix = summary.empty() ? std::string() : "\n" + summary;
#pragma endregion

#pragma region Github release retrieval
		// Repositories without releases (like a personal mod branch) answer 404 here; that is
		// normal and the commit date below still decides the result.
		bool hasRelease = false;
		Version latestRelease{};
		std::string releaseUrl;

		HttpResponse releaseResponse = HttpGet("https://api.github.com/repos/" + std::string(info.repo) + "/releases/latest");
		if (releaseResponse.code == CURLE_OK && releaseResponse.status == 200)
		{
			auto releaseJson = nlohmann::json::parse(releaseResponse.body, nullptr, false);
			if (!releaseJson.is_discarded())
			{
				const std::string tag = releaseJson.value("tag_name", "");
				if (!tag.empty())
				{
					hasRelease = true;
					latestRelease = Version::Parse(tag);
					releaseUrl = releaseJson.value("html_url", std::string(info.repoUrl));
				}
			}
		}
#pragma endregion

		if (hasRelease && latestRelease > currVersion)
		{
			std::cout << "  Update available: " << latestRelease.ToString() << std::endl;
			return { UpdateStatus::UPDATE_AVAILABLE, releaseUrl,
				"A new release (" + latestRelease.ToString() + ") is available." };
		}

		// No release past this build, so the branch's newest commit decides. A commit from the
		// same day the build was made is not treated as newer, which avoids nagging about the
		// commit the user just built from.
		const std::string buildDate = BuildDate();
		const std::string commitDay = commitDate.size() >= 10 ? commitDate.substr(0, 10) : std::string();
		if (!buildDate.empty() && !commitDay.empty() && commitDay > buildDate)
		{
			std::cout << "  Update available: commits newer than this build (" << commitLabel << ")." << std::endl;
			return { UpdateStatus::UPDATE_AVAILABLE, commitUrl,
				"New commits on " + std::string(info.branch) + ": " + commitLabel + detailSuffix };
		}

		std::cout << "  Already up to date." << std::endl;
		std::cout << "  Retrieved commit: " << commitLabel << " on " << info.branch << std::endl;
		std::cout << "  Build (current) version: " << COMET_VERSION << std::endl;

		return { UpdateStatus::UP_TO_DATE, info.repoUrl,
			"Latest commit on " + std::string(info.branch) + ": " + commitLabel + detailSuffix };
	}
	catch (const std::exception& e)
	{
		std::cout << "  Exception: " << e.what() << std::endl;
		return { UpdateStatus::CHECK_ERROR, info.repoUrl, "Something went wrong while checking for an update." };
	}
	catch (...)
	{
		std::cout << "  Something went wrong while trying to check for an update." << std::endl;
		return { UpdateStatus::CHECK_ERROR, info.repoUrl, "Something went wrong while checking for an update." };
	}
}
