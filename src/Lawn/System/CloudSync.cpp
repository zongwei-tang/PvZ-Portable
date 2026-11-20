/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifdef PVZP_CLOUD_SAVE

// curl.h pulls in windows.h on Windows; without these its min and max macros break std::min and std::max below
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "CloudSync.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <system_error>
#include <curl/curl.h>

// OAuth clients this build signs in with; cloudsave.ini can name others (see CMakeLists.txt and the README)
#ifndef PVZP_GOOGLE_CLIENT_ID
#define PVZP_GOOGLE_CLIENT_ID ""
#endif
#ifndef PVZP_GOOGLE_CLIENT_SECRET
#define PVZP_GOOGLE_CLIENT_SECRET ""
#endif
#ifndef PVZP_ONEDRIVE_CLIENT_ID
#define PVZP_ONEDRIVE_CLIENT_ID ""
#endif

namespace fs = std::filesystem;

namespace CloudSync
{

namespace
{

constexpr std::string_view MANIFEST_HEADER = "PVZP-CLOUDSAVE 1";
constexpr std::string_view TOKEN_HEADER = "PVZP-CLOUDSAVE-TOKEN 1";
constexpr const char* STATE_NAME = "cloudsave-state.txt";
constexpr const char* BACKUP_DIR = "cloudsave-backup";

std::string_view Trim(std::string_view theText)
{
	const char* aSpace = " \t\r\n";
	size_t aStart = theText.find_first_not_of(aSpace);
	if (aStart == std::string_view::npos)
		return {};
	size_t anEnd = theText.find_last_not_of(aSpace);
	return theText.substr(aStart, anEnd - aStart + 1);
}

// Values are single lines; a stray newline would start a new key.
std::string OneLine(std::string_view theValue)
{
	return std::string(Trim(theValue.substr(0, theValue.find_first_of("\r\n"))));
}

bool ReadFile(const fs::path& thePath, std::string& theData)
{
	std::ifstream aFile(thePath, std::ios::binary);
	if (!aFile)
		return false;
	std::ostringstream aStream;
	aStream << aFile.rdbuf();
	theData = std::move(aStream).str();
	return !aFile.bad();
}

bool WriteFile(const fs::path& thePath, std::string_view theData)
{
	std::ofstream aFile(thePath, std::ios::binary | std::ios::trunc);
	if (!aFile)
		return false;
	aFile.write(theData.data(), static_cast<std::streamsize>(theData.size()));
	aFile.close();
	return !aFile.fail();
}

// Writes next to the target first so a crash never leaves a half-written file behind. thePrivate keeps
// files holding a password or sign-in readable by the owner only.
bool WriteFileAtomic(const fs::path& thePath, std::string_view theData, bool thePrivate = false)
{
	fs::path aTemp = thePath;
	aTemp += ".cloudtmp";
	if (!WriteFile(aTemp, theData))
		return false;
	std::error_code anError;
	if (thePrivate)
		fs::permissions(aTemp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, anError);
	fs::rename(aTemp, thePath, anError);
	if (anError)
	{
		fs::remove(aTemp, anError);
		return false;
	}
	return true;
}

int64_t Now()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool IsSuccess(long theStatus)
{
	return theStatus >= 200 && theStatus < 300;
}

// "a/b c" -> "a/b%20c/": each segment escaped, empty segments dropped, with a trailing slash
std::string FolderPath(std::string_view theFolder)
{
	std::string aPath;
	while (!theFolder.empty())
	{
		size_t aSlash = theFolder.find('/');
		std::string_view aSegment = theFolder.substr(0, aSlash);
		if (!aSegment.empty())
			aPath += UrlEscape(aSegment) + '/';
		theFolder = aSlash == std::string_view::npos ? std::string_view() : theFolder.substr(aSlash + 1);
	}
	return aPath;
}

std::string BlobPath(const std::string& theName, uint64_t theHash)
{
	return std::format("files/{}.{:016x}", theName, theHash);
}

}

const char* ProviderKey(Provider theProvider)
{
	switch (theProvider)
	{
	case Provider::WEBDAV:			return "webdav";
	case Provider::GOOGLE_DRIVE:	return "google";
	case Provider::ONEDRIVE:		return "onedrive";
	}
	return "webdav";
}

const char* ProviderName(Provider theProvider)
{
	switch (theProvider)
	{
	case Provider::WEBDAV:			return "WebDAV";
	case Provider::GOOGLE_DRIVE:	return "Google Drive";
	case Provider::ONEDRIVE:		return "OneDrive";
	}
	return "WebDAV";
}

bool IsOAuth(Provider theProvider)
{
	return theProvider == Provider::GOOGLE_DRIVE || theProvider == Provider::ONEDRIVE;
}

std::string UrlEscape(std::string_view theText)
{
	std::string anOut;
	for (unsigned char c : theText)
	{
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			anOut += static_cast<char>(c);
		else
			anOut += std::format("%{:02X}", c);
	}
	return anOut;
}

std::string UrlEscapePath(std::string_view thePath)
{
	std::string aPath = FolderPath(thePath);
	if (!aPath.empty() && thePath.back() != '/')
		aPath.pop_back();
	return aPath;
}

std::string DescribePath(const std::string& thePath)
{
	if (thePath == MANIFEST_PATH)
		return "the cloud manifest";
	// files/<name>.<hash> -> <name>
	std::string aName = thePath.starts_with("files/") ? thePath.substr(6) : thePath;
	size_t aDot = aName.rfind('.');
	if (aDot != std::string::npos && aName.size() - aDot == 17)
		aName.resize(aDot);
	return aName;
}

bool LoadConfig(const fs::path& thePath, Config& theConfig)
{
	std::string aText;
	if (!ReadFile(thePath, aText))
		return false;

	std::istringstream aStream(aText);
	std::string aLine;
	while (std::getline(aStream, aLine))
	{
		std::string_view aView = Trim(aLine);
		if (aView.empty() || aView[0] == '#' || aView[0] == ';')
			continue;
		size_t anEquals = aView.find('=');
		if (anEquals == std::string_view::npos)
			continue;

		std::string_view aKey = Trim(aView.substr(0, anEquals));
		std::string aValue(Trim(aView.substr(anEquals + 1)));
		if (aKey == "enabled")
			theConfig.mEnabled = aValue == "1" || aValue == "true" || aValue == "yes" || aValue == "on";
		else if (aKey == "provider")
		{
			for (int i = 0; i < NUM_PROVIDERS; i++)
			{
				if (aValue == ProviderKey(static_cast<Provider>(i)))
					theConfig.mProvider = static_cast<Provider>(i);
			}
		}
		else if (aKey == "url")
			theConfig.mUrl = aValue;
		else if (aKey == "username")
			theConfig.mUsername = aValue;
		else if (aKey == "password")
			theConfig.mPassword = aValue;
		else if (aKey == "folder")
			theConfig.mFolder = aValue;
		else if (aKey == "google_client_id")
			theConfig.mGoogleClientId = aValue;
		else if (aKey == "google_client_secret")
			theConfig.mGoogleClientSecret = aValue;
		else if (aKey == "onedrive_client_id")
			theConfig.mOneDriveClientId = aValue;
	}

	if (!theConfig.mUrl.empty() && theConfig.mUrl.back() != '/')
		theConfig.mUrl += '/';
	if (theConfig.mProvider == Provider::WEBDAV && theConfig.mUrl.empty())
		theConfig.mEnabled = false;
	return true;
}

bool WriteConfig(const fs::path& thePath, const Config& theConfig)
{
	return WriteFileAtomic(thePath, std::format(
		"# PvZ-Portable cloud save. Edit this in the game (Options on the main menu, Cloud Save), or here and restart the game.\n"
		"# provider is webdav, google or onedrive.\n"
		"#\n"
		"# WebDAV (Nextcloud, ownCloud, Jianguoyun, Synology, Apache mod_dav, ...): url is the WebDAV root of your account,\n"
		"# e.g. https://cloud.example.com/remote.php/dav/files/alice/ . The password is stored here as plain text: prefer an\n"
		"# app password over your account password.\n"
		"#\n"
		"# Google Drive and OneDrive sign in through the browser on this computer, from the game. A build made without client\n"
		"# IDs of its own needs them here (see the README): google_client_id and google_client_secret of a Google OAuth client\n"
		"# of the type \"Desktop app\", onedrive_client_id of a Microsoft app registration with the platform \"Mobile and\n"
		"# desktop applications\" and the redirect URI http://localhost .\n"
		"enabled={}\n"
		"provider={}\n"
		"url={}\n"
		"username={}\n"
		"password={}\n"
		"folder={}\n"
		"google_client_id={}\n"
		"google_client_secret={}\n"
		"onedrive_client_id={}\n",
		theConfig.mEnabled ? 1 : 0, ProviderKey(theConfig.mProvider), OneLine(theConfig.mUrl), OneLine(theConfig.mUsername),
		OneLine(theConfig.mPassword), OneLine(theConfig.mFolder), OneLine(theConfig.mGoogleClientId),
		OneLine(theConfig.mGoogleClientSecret), OneLine(theConfig.mOneDriveClientId)), true);
}

bool WriteConfigTemplate(const fs::path& thePath)
{
	return WriteConfig(thePath, Config());
}

std::string GoogleClientId(const Config& theConfig)
{
	return theConfig.mGoogleClientId.empty() ? PVZP_GOOGLE_CLIENT_ID : theConfig.mGoogleClientId;
}

std::string GoogleClientSecret(const Config& theConfig)
{
	// the secret belongs to the client: a built-in secret is no use with a client ID from cloudsave.ini
	return theConfig.mGoogleClientId.empty() ? PVZP_GOOGLE_CLIENT_SECRET : theConfig.mGoogleClientSecret;
}

std::string OneDriveClientId(const Config& theConfig)
{
	return theConfig.mOneDriveClientId.empty() ? PVZP_ONEDRIVE_CLIENT_ID : theConfig.mOneDriveClientId;
}

std::string ClientId(const Config& theConfig)
{
	switch (theConfig.mProvider)
	{
	case Provider::GOOGLE_DRIVE:	return GoogleClientId(theConfig);
	case Provider::ONEDRIVE:		return OneDriveClientId(theConfig);
	case Provider::WEBDAV:			break;
	}
	return std::string();
}

bool HasBuiltInClientId(Provider theProvider)
{
	switch (theProvider)
	{
	case Provider::GOOGLE_DRIVE:	return std::string_view(PVZP_GOOGLE_CLIENT_ID).size() > 0;
	case Provider::ONEDRIVE:		return std::string_view(PVZP_ONEDRIVE_CLIENT_ID).size() > 0;
	case Provider::WEBDAV:			return false;
	}
	return false;
}

bool IsSyncedFileName(std::string_view theName)
{
	static const std::regex aPattern(R"(users\.dat|user[0-9]+\.dat|game[0-9]+_[0-9]+(_slot[0-9]+)?\.(v4|info|dat))");
	return std::regex_match(theName.begin(), theName.end(), aPattern);
}

uint64_t HashBytes(std::string_view theData)
{
	// FNV-1a: only detects changes, it is not meant to resist tampering
	uint64_t aHash = 0xcbf29ce484222325ULL;
	for (unsigned char c : theData)
	{
		aHash ^= c;
		aHash *= 0x100000001b3ULL;
	}
	return aHash;
}

std::string WriteManifest(const Manifest& theManifest)
{
	std::string aText(MANIFEST_HEADER);
	aText += '\n';
	aText += std::format("rev {}\n", theManifest.mRev);
	aText += std::format("time {}\n", theManifest.mTime);
	aText += std::format("device {}\n", theManifest.mDevice);
	std::istringstream aSummary(theManifest.mSummary);
	std::string aLine;
	while (std::getline(aSummary, aLine))
		aText += std::format("summary {}\n", aLine);
	for (const auto& [aName, anEntry] : theManifest.mFiles)
		aText += std::format("file {} {} {:016x}\n", aName, anEntry.mSize, anEntry.mHash);
	return aText;
}

bool ParseManifest(std::string_view theText, Manifest& theManifest)
{
	theManifest = Manifest();
	std::istringstream aStream{std::string(theText)};
	std::string aLine;
	if (!std::getline(aStream, aLine) || Trim(aLine) != MANIFEST_HEADER)
		return false;

	while (std::getline(aStream, aLine))
	{
		if (!aLine.empty() && aLine.back() == '\r')
			aLine.pop_back();
		size_t aSpace = aLine.find(' ');
		std::string_view aKey = std::string_view(aLine).substr(0, aSpace);
		std::string aValue = aSpace == std::string::npos ? std::string() : aLine.substr(aSpace + 1);

		if (aKey == "rev")
			theManifest.mRev = std::strtoull(aValue.c_str(), nullptr, 10);
		else if (aKey == "time")
			theManifest.mTime = std::strtoll(aValue.c_str(), nullptr, 10);
		else if (aKey == "device")
			theManifest.mDevice = aValue;
		else if (aKey == "summary")
		{
			if (!theManifest.mSummary.empty())
				theManifest.mSummary += '\n';
			theManifest.mSummary += aValue;
		}
		else if (aKey == "file")
		{
			std::istringstream aFields(aValue);
			std::string aName, aHash;
			FileEntry anEntry;
			if (!(aFields >> aName >> anEntry.mSize >> aHash))
				return false;
			// the names become local paths, so anything but a known save file name is refused
			if (!IsSyncedFileName(aName) || aHash.size() != 16)
				return false;
			anEntry.mHash = std::strtoull(aHash.c_str(), nullptr, 16);
			theManifest.mFiles[aName] = anEntry;
		}
	}
	return true;
}

FileMap DescribeSnapshot(const Snapshot& theSnapshot)
{
	FileMap aFiles;
	for (const auto& [aName, aData] : theSnapshot)
		aFiles[aName] = FileEntry{ aData.size(), HashBytes(aData) };
	return aFiles;
}

bool SameCommit(const Manifest& theA, const Manifest& theB)
{
	// not just the revision: two devices racing on a server without conditional writes can both commit the same one
	return theA.mRev == theB.mRev && theA.mTime == theB.mTime && theA.mDevice == theB.mDevice && theA.mFiles == theB.mFiles;
}

Action Decide(const Manifest* theBase, const FileMap& theLocal, const Manifest* theRemote)
{
	if (theRemote == nullptr || (theBase == nullptr && theRemote->mFiles.empty()))
		return theLocal.empty() ? Action::NONE : Action::PUSH;
	if (theLocal == theRemote->mFiles)
		return Action::NONE;
	if (theBase == nullptr)
		return theLocal.empty() ? Action::PULL : Action::CONFLICT;

	bool aLocalChanged = theLocal != theBase->mFiles;
	bool aRemoteChanged = !SameCommit(*theRemote, *theBase);
	// Every commit made on top of the base has a higher revision. Another one with the same or a lower revision was
	// made at the same time as the base, on a server that could not turn one of them down, or is an older copy: the
	// cloud lost what this device has, so ask instead of loading over it.
	if (aRemoteChanged && theRemote->mRev <= theBase->mRev)
		return Action::CONFLICT;
	if (aLocalChanged && aRemoteChanged)
		return Action::CONFLICT;
	if (aLocalChanged)
		return Action::PUSH;
	if (aRemoteChanged)
		return Action::PULL;
	return Action::NONE;
}

const char* ResultName(Result theResult)
{
	switch (theResult)
	{
	case Result::OK:			return "ok";
	case Result::DISABLED:		return "disabled";
	case Result::OFFLINE:		return "offline";
	case Result::FAILED:		return "failed";
	case Result::CONFLICT:		return "conflict";
	case Result::PULL_NEEDED:	return "pull needed";
	case Result::PUSH_NEEDED:	return "push needed";
	}
	return "?";
}

void GlobalInit()
{
	static bool aDone = false;
	if (!aDone)
	{
		curl_global_init(CURL_GLOBAL_DEFAULT);
		aDone = true;
	}
}

Result Store::Fail(Result theResult, std::string theError)
{
	mError = std::move(theError);
	return theResult;
}

namespace
{

class CurlTransport : public Transport
{
	CURL*					mCurl = nullptr;
	std::function<bool()>	mStop;

public:
	explicit CurlTransport(std::function<bool()> theStop) : mStop(std::move(theStop)) { mCurl = curl_easy_init(); }
	~CurlTransport() override { if (mCurl) curl_easy_cleanup(mCurl); }

#if LIBCURL_VERSION_NUM >= 0x072000
	static int OnProgress(void* theUser, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		return (*static_cast<const std::function<bool()>*>(theUser))() ? 1 : 0;
	}
#endif

	static size_t OnBody(char* theData, size_t theSize, size_t theCount, void* theUser)
	{
		static_cast<std::string*>(theUser)->append(theData, theSize * theCount);
		return theSize * theCount;
	}

	static size_t OnHeader(char* theData, size_t theSize, size_t theCount, void* theUser)
	{
		std::string_view aLine(theData, theSize * theCount);
		std::string& anETag = *static_cast<std::string*>(theUser);
		if (aLine.starts_with("HTTP/"))
			anETag.clear();		// a new response, after a redirect
		size_t aColon = aLine.find(':');
		if (aColon != std::string_view::npos)
		{
			std::string aKey(aLine.substr(0, aColon));
			std::transform(aKey.begin(), aKey.end(), aKey.begin(), [](unsigned char c) { return std::tolower(c); });
			if (aKey == "etag")
				anETag = std::string(Trim(aLine.substr(aColon + 1)));
		}
		return theSize * theCount;
	}

	HttpResponse Request(const HttpRequest& theRequest) override
	{
		HttpResponse aResponse;
		if (mCurl == nullptr)
		{
			aResponse.mError = "libcurl could not be initialized";
			return aResponse;
		}

		curl_easy_reset(mCurl);
		curl_easy_setopt(mCurl, CURLOPT_URL, theRequest.mUrl.c_str());
		curl_easy_setopt(mCurl, CURLOPT_USERAGENT, "PvZ-Portable cloud save");
		curl_easy_setopt(mCurl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(mCurl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(mCurl, CURLOPT_TIMEOUT, 30L);
#if LIBCURL_VERSION_NUM >= 0x075500
		curl_easy_setopt(mCurl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
		curl_easy_setopt(mCurl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
		if (theRequest.mFollowRedirects)
		{
			// credentials are not passed on to another host (no CURLOPT_UNRESTRICTED_AUTH)
			curl_easy_setopt(mCurl, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(mCurl, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
			curl_easy_setopt(mCurl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
			curl_easy_setopt(mCurl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
		}

		curl_slist* aHeaders = nullptr;
		if (!theRequest.mUsername.empty())
		{
			curl_easy_setopt(mCurl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
			curl_easy_setopt(mCurl, CURLOPT_USERNAME, theRequest.mUsername.c_str());
			curl_easy_setopt(mCurl, CURLOPT_PASSWORD, theRequest.mPassword.c_str());
		}
		else if (!theRequest.mBearer.empty())
		{
#if LIBCURL_VERSION_NUM >= 0x073D00
			curl_easy_setopt(mCurl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BEARER));
			curl_easy_setopt(mCurl, CURLOPT_XOAUTH2_BEARER, theRequest.mBearer.c_str());
#else
			aHeaders = curl_slist_append(aHeaders, ("Authorization: Bearer " + theRequest.mBearer).c_str());
#endif
		}
		curl_easy_setopt(mCurl, CURLOPT_WRITEFUNCTION, &CurlTransport::OnBody);
		curl_easy_setopt(mCurl, CURLOPT_WRITEDATA, &aResponse.mBody);
		curl_easy_setopt(mCurl, CURLOPT_HEADERFUNCTION, &CurlTransport::OnHeader);
		curl_easy_setopt(mCurl, CURLOPT_HEADERDATA, &aResponse.mETag);
#if LIBCURL_VERSION_NUM >= 0x072000
		if (mStop)
		{
			curl_easy_setopt(mCurl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(mCurl, CURLOPT_XFERINFOFUNCTION, &CurlTransport::OnProgress);
			curl_easy_setopt(mCurl, CURLOPT_XFERINFODATA, &mStop);
		}
#endif

		if (theRequest.mMethod == "GET")
			curl_easy_setopt(mCurl, CURLOPT_HTTPGET, 1L);
		else
			curl_easy_setopt(mCurl, CURLOPT_CUSTOMREQUEST, theRequest.mMethod.c_str());
		if (theRequest.mBody)
		{
			curl_easy_setopt(mCurl, CURLOPT_POSTFIELDS, theRequest.mBody->data());
			curl_easy_setopt(mCurl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(theRequest.mBody->size()));
			std::string aType = theRequest.mContentType.empty() ? "application/octet-stream" : theRequest.mContentType;
			aHeaders = curl_slist_append(aHeaders, ("Content-Type: " + aType).c_str());
			aHeaders = curl_slist_append(aHeaders, "Expect:");	// no 100-continue round trip
		}
		for (const std::string& aHeader : theRequest.mHeaders)
			aHeaders = curl_slist_append(aHeaders, aHeader.c_str());
		curl_easy_setopt(mCurl, CURLOPT_HTTPHEADER, aHeaders);

		CURLcode aCode = curl_easy_perform(mCurl);
		curl_slist_free_all(aHeaders);
		if (aCode != CURLE_OK)
		{
			aResponse.mError = aCode == CURLE_ABORTED_BY_CALLBACK ? "stopped" : curl_easy_strerror(aCode);
			return aResponse;
		}
		curl_easy_getinfo(mCurl, CURLINFO_RESPONSE_CODE, &aResponse.mStatus);
		return aResponse;
	}
};

// <url>/<folder>/, with basic authentication and If-Match on the manifest.
class WebDavStore : public Store
{
	Config						mConfig;
	std::unique_ptr<Transport>	mHttp;
	bool						mFoldersReady = false;

	std::string Base() const
	{
		return mConfig.mUrl + FolderPath(mConfig.mFolder);
	}

	HttpResponse Send(const char* theMethod, const std::string& theUrl, const std::string* theBody = nullptr, std::vector<std::string> theHeaders = {})
	{
		HttpRequest aRequest;
		aRequest.mMethod = theMethod;
		aRequest.mUrl = theUrl;
		aRequest.mBody = theBody;
		aRequest.mHeaders = std::move(theHeaders);
		aRequest.mUsername = mConfig.mUsername;
		aRequest.mPassword = mConfig.mPassword;
		return mHttp->Request(aRequest);
	}

	Result Check(const HttpResponse& theResponse, std::string_view theWhat)
	{
		if (theResponse.mStatus == 0)
			return Fail(Result::OFFLINE, theResponse.mError);
		if (theResponse.mStatus == 401 || theResponse.mStatus == 403)
			return Fail(Result::FAILED, "the server rejected the username or password");
		if (!IsSuccess(theResponse.mStatus))
			return Fail(Result::FAILED, std::format("{} failed with HTTP {}", theWhat, theResponse.mStatus));
		return Result::OK;
	}

public:
	WebDavStore(const Config& theConfig, std::unique_ptr<Transport> theTransport) : mConfig(theConfig), mHttp(std::move(theTransport)) {}

	// the same key the sync state was saved under before there were other providers
	std::string Id() const override { return Base(); }

	Result Get(const std::string& thePath, bool& theFound, std::string& theBody, std::string& theVersion) override
	{
		theFound = false;
		HttpResponse aResponse = Send("GET", Base() + UrlEscapePath(thePath));
		if (aResponse.mStatus == 404)
			return Result::OK;
		Result aResult = Check(aResponse, "downloading " + DescribePath(thePath));
		if (aResult != Result::OK)
			return aResult;
		theFound = true;
		theBody = std::move(aResponse.mBody);
		theVersion = aResponse.mETag;
		return Result::OK;
	}

	Result Put(const std::string& thePath, const std::string& theBody, Condition theCondition, const std::string& theVersion) override
	{
		std::vector<std::string> aHeaders;
		if (theCondition == Condition::ABSENT)
			aHeaders.push_back("If-None-Match: *");
		else if (theCondition == Condition::VERSION && !theVersion.empty() && !theVersion.starts_with("W/"))	// a weak ETag never matches If-Match
			aHeaders.push_back("If-Match: " + theVersion);

		HttpResponse aResponse = Send("PUT", Base() + UrlEscapePath(thePath), &theBody, std::move(aHeaders));
		if (aResponse.mStatus == 412)
			return Fail(Result::CONFLICT, "another device saved to the cloud at the same time");
		return Check(aResponse, "uploading " + DescribePath(thePath));
	}

	Result Delete(const std::string& thePath) override
	{
		HttpResponse aResponse = Send("DELETE", Base() + UrlEscapePath(thePath));
		if (aResponse.mStatus == 404)
			return Result::OK;
		return Check(aResponse, "deleting " + DescribePath(thePath));
	}

	Result Prepare() override
	{
		if (mFoldersReady)
			return Result::OK;

		std::vector<std::string> aFolders;
		std::string aPath = FolderPath(mConfig.mFolder);
		for (size_t aSlash = aPath.find('/'); aSlash != std::string::npos; aSlash = aPath.find('/', aSlash + 1))
			aFolders.push_back(aPath.substr(0, aSlash + 1));
		aFolders.push_back(aPath + "files/");

		for (const std::string& aFolder : aFolders)
		{
			// 405 means the collection already exists
			HttpResponse aResponse = Send("MKCOL", mConfig.mUrl + aFolder);
			if (aResponse.mStatus == 405)
				continue;
			Result aResult = Check(aResponse, "creating the cloud folder");
			if (aResult != Result::OK)
				return aResult;
		}
		mFoldersReady = true;
		return Result::OK;
	}
};

}

std::unique_ptr<Transport> MakeCurlTransport(std::function<bool()> theStop)
{
	return std::make_unique<CurlTransport>(std::move(theStop));
}

std::unique_ptr<Store> MakeWebDavStore(const Config& theConfig, std::unique_ptr<Transport> theTransport)
{
	return std::make_unique<WebDavStore>(theConfig, std::move(theTransport));
}

std::unique_ptr<Store> MakeStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport)
{
	if (!theTransport)
		theTransport = MakeCurlTransport();
	switch (theConfig.mProvider)
	{
	case Provider::GOOGLE_DRIVE:	return MakeGoogleDriveStore(theConfig, theStateDir, std::move(theTransport));
	case Provider::ONEDRIVE:		return MakeOneDriveStore(theConfig, theStateDir, std::move(theTransport));
	case Provider::WEBDAV:			break;
	}
	return MakeWebDavStore(theConfig, std::move(theTransport));
}

namespace
{

std::mutex gTokenMutex;		// for all the token files

bool ReadToken(const fs::path& thePath, Token& theToken)
{
	theToken = Token();
	std::string aText;
	if (!ReadFile(thePath, aText))
		return false;

	std::istringstream aStream(aText);
	std::string aLine;
	if (!std::getline(aStream, aLine) || Trim(aLine) != TOKEN_HEADER)
		return false;
	while (std::getline(aStream, aLine))
	{
		std::string_view aView = Trim(aLine);
		size_t aSpace = aView.find(' ');
		std::string_view aKey = aView.substr(0, aSpace);
		std::string aValue = aSpace == std::string_view::npos ? std::string() : std::string(Trim(aView.substr(aSpace + 1)));
		if (aKey == "refresh")
			theToken.mRefresh = aValue;
		else if (aKey == "account")
			theToken.mAccount = aValue;
		else if (aKey == "name")
			theToken.mAccountName = aValue;
		else if (aKey == "client")
			theToken.mClient = aValue;
	}
	return !theToken.mRefresh.empty();
}

// Earlier versions of the game skip the client line, as any key they do not know.
bool WriteToken(const fs::path& thePath, const Token& theToken)
{
	std::error_code anError;
	fs::create_directories(thePath.parent_path(), anError);
	return WriteFileAtomic(thePath, std::format("{}\naccount {}\nname {}\nclient {}\nrefresh {}\n", TOKEN_HEADER, OneLine(theToken.mAccount),
		OneLine(theToken.mAccountName), OneLine(theToken.mClient), OneLine(theToken.mRefresh)), true);
}

}

fs::path TokenPath(const fs::path& theStateDir, Provider theProvider)
{
	return theStateDir / std::format("cloudsave-{}-token.txt", ProviderKey(theProvider));
}

bool LoadToken(const fs::path& theStateDir, Provider theProvider, Token& theToken)
{
	std::lock_guard aLock(gTokenMutex);
	return ReadToken(TokenPath(theStateDir, theProvider), theToken);
}

bool LoadTokenFor(const fs::path& theStateDir, const Config& theConfig, Token& theToken)
{
	if (LoadToken(theStateDir, theConfig.mProvider, theToken) && (theToken.mClient.empty() || theToken.mClient == ClientId(theConfig)))
		return true;
	theToken = Token();
	return false;
}

bool SaveToken(const fs::path& theStateDir, Provider theProvider, const Token& theToken)
{
	std::lock_guard aLock(gTokenMutex);
	return WriteToken(TokenPath(theStateDir, theProvider), theToken);
}

bool ReplaceToken(const fs::path& theStateDir, Provider theProvider, const std::string& theOld, const Token& theToken)
{
	std::lock_guard aLock(gTokenMutex);
	fs::path aPath = TokenPath(theStateDir, theProvider);
	Token aSaved;
	return ReadToken(aPath, aSaved) && aSaved.mRefresh == theOld && WriteToken(aPath, theToken);
}

void DeleteToken(const fs::path& theStateDir, Provider theProvider)
{
	std::lock_guard aLock(gTokenMutex);
	std::error_code anError;
	fs::remove(TokenPath(theStateDir, theProvider), anError);
}

Engine::Engine(const Config& theConfig, fs::path theDataDir, fs::path theStateDir, std::string theDevice, std::unique_ptr<Store> theStore) :
	mConfig(theConfig), mDataDir(std::move(theDataDir)), mStateDir(std::move(theStateDir)), mDevice(std::move(theDevice)),
	mStore(theStore ? std::move(theStore) : MakeStore(theConfig, mStateDir))
{
}

Result Engine::Fail(Result theResult, std::string theError)
{
	mLastError = std::move(theError);
	return theResult;
}

Result Engine::StoreFailed(Result theResult)
{
	mLastError = mStore->GetError();
	return theResult;
}

Snapshot Engine::ReadLocal() const
{
	Snapshot aSnapshot;
	std::error_code anError;
	for (const fs::directory_entry& anEntry : fs::directory_iterator(mDataDir, anError))
	{
		std::string aName = anEntry.path().filename().string();
		if (!anEntry.is_regular_file() || !IsSyncedFileName(aName))
			continue;
		std::string aData;
		if (ReadFile(anEntry.path(), aData))
			aSnapshot[aName] = std::move(aData);
	}
	return aSnapshot;
}

int64_t Engine::LocalTime() const
{
	using namespace std::chrono;
	int64_t aNewest = 0;
	std::error_code anError;
	for (const fs::directory_entry& anEntry : fs::directory_iterator(mDataDir, anError))
	{
		if (!anEntry.is_regular_file() || !IsSyncedFileName(anEntry.path().filename().string()))
			continue;
		fs::file_time_type aTime = anEntry.last_write_time(anError);
		if (anError)
			continue;
		auto aSystemTime = time_point_cast<system_clock::duration>(aTime - fs::file_time_type::clock::now() + system_clock::now());
		aNewest = std::max<int64_t>(aNewest, duration_cast<seconds>(aSystemTime.time_since_epoch()).count());
	}
	return aNewest;
}

bool Engine::LoadBase(Manifest& theBase) const
{
	// The first line names the cloud folder the base belongs to, so pointing the game at another one starts over.
	std::string aText;
	if (!ReadFile(mStateDir / STATE_NAME, aText))
		return false;
	size_t aNewline = aText.find('\n');
	if (aNewline == std::string::npos || aText.substr(0, aNewline) != mStore->Id())
		return false;
	return ParseManifest(std::string_view(aText).substr(aNewline + 1), theBase);
}

void Engine::SaveBase(const Manifest& theBase) const
{
	std::error_code anError;
	fs::create_directories(mStateDir, anError);
	WriteFileAtomic(mStateDir / STATE_NAME, mStore->Id() + '\n' + WriteManifest(theBase));
}

void Engine::Backup(const Snapshot& theSnapshot, std::string_view theSide)
{
	if (theSnapshot.empty())
		return;

	std::time_t aNow = std::time(nullptr);
	std::tm aTm{};
#ifdef _WIN32
	localtime_s(&aTm, &aNow);
#else
	localtime_r(&aNow, &aTm);
#endif
	char aStamp[32];
	std::strftime(aStamp, sizeof(aStamp), "%Y%m%d-%H%M%S", &aTm);

	fs::path aDir = mStateDir / BACKUP_DIR / std::format("{}-{}", aStamp, theSide);
	std::error_code anError;
	fs::create_directories(aDir, anError);
	for (const auto& [aName, aData] : theSnapshot)
		WriteFile(aDir / aName, aData);
	mLastBackup = aDir;
}

Result Engine::FetchRemote(Remote& theRemote)
{
	theRemote = Remote();
	bool aFound = false;
	std::string aBody;
	Result aResult = mStore->Get(MANIFEST_PATH, aFound, aBody, theRemote.mVersion);
	if (aResult != Result::OK)
		return StoreFailed(aResult);
	if (!aFound)
		return Result::OK;
	if (!ParseManifest(aBody, theRemote.mManifest))
		return Fail(Result::FAILED, "the cloud manifest is damaged");
	theRemote.mExists = true;
	return Result::OK;
}

Result Engine::Push(const Snapshot& theLocal, const std::string& theSummary, const Remote& theRemote, bool theUploadAll)
{
	Result aResult = mStore->Prepare();
	if (aResult != Result::OK)
		return StoreFailed(aResult);

	FileMap aLocal = DescribeSnapshot(theLocal);
	const FileMap& aRemoteFiles = theRemote.mManifest.mFiles;
	for (const auto& [aName, anEntry] : aLocal)
	{
		auto anItr = aRemoteFiles.find(aName);
		if (!theUploadAll && anItr != aRemoteFiles.end() && anItr->second == anEntry)
			continue;	// that blob is already in the cloud

		aResult = mStore->Put(BlobPath(aName, anEntry.mHash), theLocal.at(aName));
		if (aResult != Result::OK)
			return StoreFailed(aResult);
	}

	// Not every server can write conditionally, so look once more right before committing.
	Remote aLatest;
	aResult = FetchRemote(aLatest);
	if (aResult != Result::OK)
		return aResult;
	if (aLatest.mExists != theRemote.mExists || (aLatest.mExists && !SameCommit(aLatest.mManifest, theRemote.mManifest)))
	{
		mConflict = ConflictInfo{ aLatest.mManifest, LocalTime() };
		return Fail(Result::CONFLICT, "another device saved to the cloud at the same time");
	}

	Manifest aBase;
	bool aHasBase = LoadBase(aBase);
	Manifest aManifest;
	aManifest.mRev = std::max(theRemote.mManifest.mRev, aHasBase ? aBase.mRev : 0) + 1;
	aManifest.mTime = Now();
	aManifest.mDevice = mDevice;
	aManifest.mSummary = theSummary;
	aManifest.mFiles = aLocal;

	std::string aText = WriteManifest(aManifest);
	for (int aTry = 0;; aTry++)
	{
		aResult = mStore->Put(MANIFEST_PATH, aText, aLatest.mExists ? Store::Condition::VERSION : Store::Condition::ABSENT, aLatest.mVersion);
		if (aResult != Result::CONFLICT)
			break;

		Remote aWinner;
		Result aFetched = FetchRemote(aWinner);
		if (aFetched != Result::OK)
			return aFetched;	// what is in the cloud now is not known: try again later rather than ask about it
		// Still the same commit, only under a new version (OneDrive also changes the eTag of a file when something
		// else than its content changes): commit on top of that once more.
		if (aTry == 0 && aLatest.mExists && aWinner.mExists && SameCommit(aWinner.mManifest, aLatest.mManifest) && aWinner.mVersion != aLatest.mVersion)
		{
			aLatest = std::move(aWinner);
			continue;
		}
		mConflict = ConflictInfo{ aWinner.mManifest, LocalTime() };
		return Fail(Result::CONFLICT, "another device saved to the cloud at the same time");
	}
	if (aResult != Result::OK)
		return StoreFailed(aResult);

	SaveBase(aManifest);

	// Blobs only the replaced manifest pointed at are garbage now; a failed delete just leaves a stray file.
	for (const auto& [aName, anEntry] : aRemoteFiles)
	{
		auto anItr = aLocal.find(aName);
		if (anItr == aLocal.end() || anItr->second.mHash != anEntry.mHash)
			mStore->Delete(BlobPath(aName, anEntry.mHash));
	}
	return Result::OK;
}

Result Engine::Download(const Manifest& theManifest, const Snapshot& theHave, Snapshot& theOut, bool theSkipBroken)
{
	theOut.clear();
	for (const auto& [aName, anEntry] : theManifest.mFiles)
	{
		auto aHave = theHave.find(aName);
		if (aHave != theHave.end() && aHave->second.size() == anEntry.mSize && HashBytes(aHave->second) == anEntry.mHash)
		{
			theOut[aName] = aHave->second;
			continue;
		}

		bool aFound = false;
		std::string aData, aVersion;
		Result aResult = mStore->Get(BlobPath(aName, anEntry.mHash), aFound, aData, aVersion);
		if (aResult != Result::OK)
			return StoreFailed(aResult);
		bool aBroken = !aFound || aData.size() != anEntry.mSize || HashBytes(aData) != anEntry.mHash;
		if (aBroken && theSkipBroken)
			continue;
		if (!aFound)
			return Fail(Result::FAILED, std::format("the cloud copy of {} is missing", aName));
		if (aBroken)
			return Fail(Result::FAILED, std::format("the cloud copy of {} does not match its manifest", aName));
		theOut[aName] = std::move(aData);
	}
	return Result::OK;
}

Result Engine::Pull(const Remote& theRemote, const Snapshot& theLocal, bool theBackupLocal)
{
	// Everything is downloaded and checked before the first local file changes.
	Snapshot aFiles;
	Result aResult = Download(theRemote.mManifest, theLocal, aFiles);
	if (aResult != Result::OK)
		return aResult;

	if (theBackupLocal)
		Backup(theLocal, "local");

	std::error_code anError;
	fs::create_directories(mDataDir, anError);
	for (const auto& [aName, aData] : aFiles)
	{
		auto aHave = theLocal.find(aName);
		if (aHave != theLocal.end() && aHave->second == aData)
			continue;
		if (!WriteFileAtomic(mDataDir / aName, aData))
			return Fail(Result::FAILED, std::format("could not write {}", aName));
	}
	for (const auto& [aName, aData] : theLocal)
	{
		if (aFiles.find(aName) == aFiles.end())
			fs::remove(mDataDir / aName, anError);
	}

	SaveBase(theRemote.mManifest);
	return Result::OK;
}

Result Engine::Sync(const Snapshot& theLocal, const std::string& theSummary, bool theAllowPull, bool theAllowPush)
{
	if (!mConfig.mEnabled)
		return Result::DISABLED;

	Remote aRemote;
	Result aResult = FetchRemote(aRemote);
	if (aResult != Result::OK)
		return aResult;

	Manifest aBase;
	bool aHasBase = LoadBase(aBase);
	FileMap aLocal = DescribeSnapshot(theLocal);
	switch (Decide(aHasBase ? &aBase : nullptr, aLocal, aRemote.mExists ? &aRemote.mManifest : nullptr))
	{
	case Action::NONE:
		// both sides already agree, possibly after the other device pushed the same files
		if (aRemote.mExists && aLocal == aRemote.mManifest.mFiles && (!aHasBase || !SameCommit(aBase, aRemote.mManifest)))
			SaveBase(aRemote.mManifest);
		return Result::OK;

	case Action::PUSH:
		if (!theAllowPush)
			return Result::PUSH_NEEDED;
		return Push(theLocal, theSummary, aRemote);

	case Action::PULL:
		if (!theAllowPull)
		{
			mConflict = ConflictInfo{ aRemote.mManifest, LocalTime() };
			return Result::PULL_NEEDED;
		}
		return Pull(aRemote, theLocal, false);

	case Action::CONFLICT:
		mConflict = ConflictInfo{ aRemote.mManifest, LocalTime() };
		return Result::CONFLICT;
	}
	return Result::FAILED;
}

Result Engine::KeepLocal(const Snapshot& theLocal, const std::string& theSummary)
{
	if (!mConfig.mEnabled)
		return Result::DISABLED;

	Remote aRemote;
	Result aResult = FetchRemote(aRemote);
	if (aResult != Result::OK)
		return aResult;

	if (aRemote.mExists)
	{
		// whatever can still be read of the cloud copy is backed up, so a damaged one never stops this one from winning
		Snapshot aCloud;
		aResult = Download(aRemote.mManifest, {}, aCloud, true);
		if (aResult != Result::OK)
			return aResult;
		Backup(aCloud, "cloud");
	}
	// all files again: in a damaged cloud copy a file can be missing although its manifest entry matches this one
	return Push(theLocal, theSummary, aRemote, true);
}

Result Engine::UseCloud()
{
	if (!mConfig.mEnabled)
		return Result::DISABLED;

	Remote aRemote;
	Result aResult = FetchRemote(aRemote);
	if (aResult != Result::OK)
		return aResult;
	if (!aRemote.mExists)
		return Fail(Result::FAILED, "there is no save in the cloud yet");
	return Pull(aRemote, ReadLocal(), true);
}

}

#endif
