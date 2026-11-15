/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// Google Drive and OneDrive: OAuth device sign-in and the two stores (see CloudSync.h).

#ifdef PVZP_CLOUD_SAVE

#include "CloudSync.h"
#include "CloudJson.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <initializer_list>
#include <random>
#include <tuple>
#include <utility>

namespace fs = std::filesystem;

namespace CloudSync
{

namespace
{

using Clock = std::chrono::steady_clock;

// drive.appdata: only the app's own hidden folder in the player's Drive, nothing else in it
constexpr const char* GOOGLE_DEVICE_URL = "https://oauth2.googleapis.com/device/code";
constexpr const char* GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char* GOOGLE_SCOPE = "https://www.googleapis.com/auth/drive.appdata";
constexpr const char* GOOGLE_FILES_URL = "https://www.googleapis.com/drive/v3/files";
constexpr const char* GOOGLE_UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3/files";
constexpr const char* GOOGLE_ABOUT_URL = "https://www.googleapis.com/drive/v3/about";

// Files.ReadWrite.AppFolder: only Apps/<app name> in the player's OneDrive; offline_access gives the refresh token
constexpr const char* MICROSOFT_DEVICE_URL = "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode";
constexpr const char* MICROSOFT_TOKEN_URL = "https://login.microsoftonline.com/common/oauth2/v2.0/token";
constexpr const char* MICROSOFT_SCOPE = "Files.ReadWrite.AppFolder offline_access";
constexpr const char* GRAPH_APPROOT_URL = "https://graph.microsoft.com/v1.0/me/drive/special/approot";

constexpr const char* RACE_LOST = "another device saved to the cloud at the same time";

bool IsSuccess(long theStatus)
{
	return theStatus >= 200 && theStatus < 300;
}

struct Client
{
	std::string	mId;
	std::string	mSecret;
	const char*	mDeviceUrl;
	const char*	mTokenUrl;
	const char*	mScope;
};

Client ClientFor(const Config& theConfig)
{
	if (theConfig.mProvider == Provider::GOOGLE_DRIVE)
		return Client{ GoogleClientId(theConfig), GoogleClientSecret(theConfig), GOOGLE_DEVICE_URL, GOOGLE_TOKEN_URL, GOOGLE_SCOPE };
	return Client{ OneDriveClientId(theConfig), "", MICROSOFT_DEVICE_URL, MICROSOFT_TOKEN_URL, MICROSOFT_SCOPE };
}

// application/x-www-form-urlencoded; empty values are left out
std::string Form(std::initializer_list<std::pair<const char*, std::string>> theFields)
{
	std::string aBody;
	for (const auto& [aKey, aValue] : theFields)
	{
		if (aValue.empty())
			continue;
		if (!aBody.empty())
			aBody += '&';
		aBody += aKey;
		aBody += '=';
		aBody += UrlEscape(aValue);
	}
	return aBody;
}

HttpResponse PostForm(Transport& theHttp, const char* theUrl, const std::string& theBody)
{
	HttpRequest aRequest;
	aRequest.mMethod = "POST";
	aRequest.mUrl = theUrl;
	aRequest.mBody = &theBody;
	aRequest.mContentType = "application/x-www-form-urlencoded";
	aRequest.mHeaders.push_back("Accept: application/json");
	return theHttp.Request(aRequest);
}

// Error texts from Microsoft run over several lines of trace ids.
std::string Shorten(std::string theText)
{
	size_t anEnd = theText.find_first_of("\r\n");
	if (anEnd != std::string::npos)
		theText.resize(anEnd);
	size_t aTrace = theText.find(" Trace ID");
	if (aTrace != std::string::npos)
		theText.resize(aTrace);
	if (theText.size() > 160)
		theText = theText.substr(0, 157) + "...";
	return theText;
}

// OAuth errors are {"error": "invalid_grant", "error_description": "..."}, API errors {"error": {"message": "..."}}.
std::string ErrorMessage(const HttpResponse& theResponse)
{
	Json::Value aJson;
	if (Json::Parse(theResponse.mBody, aJson))
	{
		if (const Json::Value* anError = aJson.Find("error"))
		{
			if (anError->mType == Json::Value::Type::OBJECT)
			{
				std::string aMessage = anError->Str("message");
				if (!aMessage.empty())
					return Shorten(aMessage);
			}
			else
			{
				std::string aDescription = aJson.Str("error_description");
				return Shorten(aDescription.empty() ? anError->mString : aDescription);
			}
		}
	}
	return std::format("HTTP {}", theResponse.mStatus);
}

std::string RandomId()
{
	std::random_device aDevice;
	std::mt19937_64 aRandom((static_cast<uint64_t>(aDevice()) << 32) ^ aDevice() ^
		static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
	return std::format("{:016x}", aRandom());
}

// Best effort: a stable id of the account (and a name to show) right after the sign-in.
void FetchAccount(Transport& theHttp, Provider theProvider, const std::string& theAccess, Token& theToken)
{
	HttpRequest aRequest;
	aRequest.mBearer = theAccess;
	if (theProvider == Provider::GOOGLE_DRIVE)
		aRequest.mUrl = std::format("{}?fields={}", GOOGLE_ABOUT_URL, UrlEscape("user(permissionId,emailAddress)"));
	else
		aRequest.mUrl = std::format("{}?{}={}", GRAPH_APPROOT_URL, UrlEscape("$select"), UrlEscape("id,parentReference"));

	HttpResponse aResponse = theHttp.Request(aRequest);
	Json::Value aJson;
	if (!IsSuccess(aResponse.mStatus) || !Json::Parse(aResponse.mBody, aJson))
		return;

	if (theProvider == Provider::GOOGLE_DRIVE)
	{
		const Json::Value* aUser = aJson.Find("user");
		if (aUser && !aUser->Str("permissionId").empty())
		{
			theToken.mAccount = "google-" + aUser->Str("permissionId");
			theToken.mAccountName = aUser->Str("emailAddress");
		}
	}
	else
	{
		const Json::Value* aParent = aJson.Find("parentReference");
		std::string aDrive = aParent ? aParent->Str("driveId") : std::string();
		std::string anId = aJson.Str("id");
		if (!anId.empty())
			theToken.mAccount = aDrive.empty() ? anId : aDrive + "/" + anId;
	}
}

// Keeps the access token of a sign-in fresh. Used from one thread at a time, like the store that owns it.
class OAuthSession
{
	Config					mConfig;
	fs::path				mStateDir;
	Transport&				mHttp;
	Token					mToken;
	bool					mSignedIn = false;
	std::string				mAccess;
	Clock::time_point		mExpiry;
	std::string				mError;

	Result Fail(Result theResult, std::string theError)
	{
		mError = std::move(theError);
		return theResult;
	}

	Result Refresh()
	{
		Client aClient = ClientFor(mConfig);
		HttpResponse aResponse = PostForm(mHttp, aClient.mTokenUrl, Form({ { "client_id", aClient.mId }, { "client_secret", aClient.mSecret },
			{ "grant_type", "refresh_token" }, { "refresh_token", mToken.mRefresh } }));
		if (aResponse.mStatus == 0)
			return Fail(Result::OFFLINE, aResponse.mError);

		Json::Value aJson;
		bool aParsed = Json::Parse(aResponse.mBody, aJson);
		std::string anAccess = aParsed ? aJson.Str("access_token") : std::string();
		if (!IsSuccess(aResponse.mStatus) || anAccess.empty())
		{
			if (aParsed && aJson.Str("error") == "invalid_grant")
				return Fail(Result::FAILED, std::format("the {} sign-in expired or was revoked, sign in again", ProviderName(mConfig.mProvider)));
			return Fail(Result::FAILED, std::format("signing in to {} failed: {}", ProviderName(mConfig.mProvider), ErrorMessage(aResponse)));
		}

		mAccess = anAccess;
		int64_t aLifetime = std::clamp<int64_t>(aJson.Int("expires_in", 3600), 60, 86400);
		mExpiry = Clock::now() + std::chrono::seconds(aLifetime - 30);

		// Microsoft hands out a new refresh token with every refresh (the old one keeps working for a while)
		std::string aRefresh = aJson.Str("refresh_token");
		if (!aRefresh.empty() && aRefresh != mToken.mRefresh)
		{
			mToken.mRefresh = aRefresh;
			SaveToken(mStateDir, mConfig.mProvider, mToken);
		}
		return Result::OK;
	}

public:
	OAuthSession(const Config& theConfig, fs::path theStateDir, Transport& theHttp) :
		mConfig(theConfig), mStateDir(std::move(theStateDir)), mHttp(theHttp)
	{
		mSignedIn = LoadToken(mStateDir, mConfig.mProvider, mToken);
	}

	const Token&		GetToken() const { return mToken; }
	const std::string&	GetError() const { return mError; }

	// Sends theRequest with the access token, refreshing it once if the server turns it down.
	// OK means an answer arrived, whatever its status.
	Result Send(HttpRequest theRequest, HttpResponse& theResponse)
	{
		if (!mSignedIn)
			return Fail(Result::FAILED, std::format("not signed in to {} yet", ProviderName(mConfig.mProvider)));

		for (int aTry = 0; aTry < 2; aTry++)
		{
			if (mAccess.empty() || Clock::now() >= mExpiry)
			{
				Result aResult = Refresh();
				if (aResult != Result::OK)
					return aResult;
			}
			theRequest.mBearer = mAccess;
			theResponse = mHttp.Request(theRequest);
			if (theResponse.mStatus == 0)
				return Fail(Result::OFFLINE, theResponse.mError);
			if (theResponse.mStatus != 401)
				return Result::OK;
			mAccess.clear();	// expired early or revoked: refresh once and try again
		}
		return Fail(Result::FAILED, std::format("{} turned down the sign-in, sign in again", ProviderName(mConfig.mProvider)));
	}
};

class OAuthStore : public Store
{
protected:
	Config						mConfig;
	std::unique_ptr<Transport>	mHttp;
	OAuthSession				mAuth;

	OAuthStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport) :
		mConfig(theConfig), mHttp(std::move(theTransport)), mAuth(theConfig, theStateDir, *mHttp)
	{
	}

	Result Call(HttpRequest theRequest, HttpResponse& theResponse)
	{
		Result aResult = mAuth.Send(std::move(theRequest), theResponse);
		if (aResult != Result::OK)
			return Fail(aResult, mAuth.GetError());
		return Result::OK;
	}

	Result Check(const HttpResponse& theResponse, std::string_view theWhat)
	{
		if (theResponse.mStatus == 0)
			return Fail(Result::OFFLINE, theResponse.mError);
		if (IsSuccess(theResponse.mStatus))
			return Result::OK;
		return Fail(Result::FAILED, std::format("{} failed: {}", theWhat, ErrorMessage(theResponse)));
	}

	// the cloud folder is a prefix of every name
	std::string Name(const std::string& thePath) const
	{
		std::string aFolder = mConfig.mFolder;
		while (!aFolder.empty() && aFolder.back() == '/')
			aFolder.pop_back();
		while (!aFolder.empty() && aFolder.front() == '/')
			aFolder.erase(aFolder.begin());
		return aFolder.empty() ? thePath : aFolder + "/" + thePath;
	}

	std::string Key(const char* theProvider) const
	{
		return std::format("{}:{}:{}", theProvider, mAuth.GetToken().mAccount, Name(""));
	}
};

// The app's hidden appDataFolder. Drive addresses files by id, allows several files with one name and has no
// conditional writes, so a listing maps names to ids and the oldest file of a name is the one that counts.
class GoogleDriveStore : public OAuthStore
{
	struct File
	{
		std::string	mId;
		std::string	mCreated;
		std::string	mVersion;
		uint64_t	mSize = 0;
	};

	std::map<std::string, std::vector<File>>	mFiles;
	bool										mListed = false;

	static File ReadFile(const Json::Value& theJson)
	{
		return File{ theJson.Str("id"), theJson.Str("createdTime"), theJson.Str("version"), static_cast<uint64_t>(theJson.Int("size", 0)) };
	}

	Result List()
	{
		mFiles.clear();
		mListed = false;
		std::string aPage;
		for (int aCount = 0; aCount < 100; aCount++)
		{
			HttpRequest aRequest;
			aRequest.mUrl = std::format("{}?spaces=appDataFolder&pageSize=1000&fields={}", GOOGLE_FILES_URL,
				UrlEscape("nextPageToken,files(id,name,createdTime,version,size)"));
			if (!aPage.empty())
				aRequest.mUrl += "&pageToken=" + UrlEscape(aPage);

			HttpResponse aResponse;
			Result aResult = Call(aRequest, aResponse);
			if (aResult == Result::OK)
				aResult = Check(aResponse, "listing the Google Drive app folder");
			if (aResult != Result::OK)
				return aResult;

			Json::Value aJson;
			if (!Json::Parse(aResponse.mBody, aJson))
				return Fail(Result::FAILED, "Google Drive sent a file list that could not be read");
			if (const Json::Value* aList = aJson.Find("files"))
			{
				for (const Json::Value& anItem : aList->mArray)
				{
					File aFile = ReadFile(anItem);
					if (!aFile.mId.empty())
						mFiles[anItem.Str("name")].push_back(aFile);
				}
			}
			aPage = aJson.Str("nextPageToken");
			if (aPage.empty())
				break;
		}

		for (auto& [aName, aList] : mFiles)
			Sort(aList);
		mListed = true;
		return Result::OK;
	}

	static void Sort(std::vector<File>& theList)
	{
		// RFC 3339 times in UTC sort as text
		std::sort(theList.begin(), theList.end(), [](const File& a, const File& b) { return std::tie(a.mCreated, a.mId) < std::tie(b.mCreated, b.mId); });
	}

	bool Find(const std::string& theName, File& theFile) const
	{
		auto anItr = mFiles.find(theName);
		if (anItr == mFiles.end() || anItr->second.empty())
			return false;
		theFile = anItr->second.front();
		return true;
	}

	Result Create(const std::string& theName, const std::string& theBody, std::string_view theWhat, File& theFile)
	{
		std::string aBoundary;
		do
			aBoundary = "pvzp-" + RandomId() + RandomId();
		while (theBody.find(aBoundary) != std::string::npos);

		std::string aBody = std::format("--{0}\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n"
			"{{\"name\":{1},\"parents\":[\"appDataFolder\"]}}\r\n"
			"--{0}\r\nContent-Type: application/octet-stream\r\n\r\n", aBoundary, Json::Quote(theName));
		aBody += theBody;
		aBody += std::format("\r\n--{}--\r\n", aBoundary);

		HttpRequest aRequest;
		aRequest.mMethod = "POST";
		aRequest.mUrl = std::format("{}?uploadType=multipart&fields={}", GOOGLE_UPLOAD_URL, UrlEscape("id,createdTime,version,size"));
		aRequest.mBody = &aBody;
		aRequest.mContentType = "multipart/related; boundary=" + aBoundary;

		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult == Result::OK)
			aResult = Check(aResponse, theWhat);
		if (aResult != Result::OK)
			return aResult;

		Json::Value aJson;
		if (!Json::Parse(aResponse.mBody, aJson) || (theFile = ReadFile(aJson)).mId.empty())
			return Fail(Result::FAILED, std::format("{} failed: Google Drive sent no file id", theWhat));
		std::vector<File>& aList = mFiles[theName];
		aList.push_back(theFile);
		Sort(aList);
		return Result::OK;
	}

	Result Update(const std::string& theName, const File& theFile, const std::string& theBody, std::string_view theWhat)
	{
		HttpRequest aRequest;
		aRequest.mMethod = "PATCH";
		aRequest.mUrl = std::format("{}/{}?uploadType=media&fields={}", GOOGLE_UPLOAD_URL, UrlEscape(theFile.mId), UrlEscape("id,createdTime,version,size"));
		aRequest.mBody = &theBody;

		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult == Result::OK)
			aResult = Check(aResponse, theWhat);
		if (aResult != Result::OK)
			return aResult;

		Json::Value aJson;
		if (Json::Parse(aResponse.mBody, aJson))
		{
			for (File& aFile : mFiles[theName])
			{
				if (aFile.mId == theFile.mId)
				{
					aFile.mVersion = aJson.Str("version");
					aFile.mSize = static_cast<uint64_t>(aJson.Int("size", static_cast<int64_t>(theBody.size())));
				}
			}
		}
		return Result::OK;
	}

	Result DeleteId(const std::string& theId, std::string_view theWhat)
	{
		HttpRequest aRequest;
		aRequest.mMethod = "DELETE";
		aRequest.mUrl = std::format("{}/{}", GOOGLE_FILES_URL, UrlEscape(theId));
		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK || aResponse.mStatus == 404)
			return aResult;
		return Check(aResponse, theWhat);
	}

public:
	GoogleDriveStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport) :
		OAuthStore(theConfig, theStateDir, std::move(theTransport))
	{
	}

	std::string Id() const override { return Key("google"); }

	Result Get(const std::string& thePath, bool& theFound, std::string& theBody, std::string& theVersion) override
	{
		theFound = false;
		// every sync starts by reading the manifest, so the listing is fresh for it
		if (!mListed || thePath == MANIFEST_PATH)
		{
			Result aResult = List();
			if (aResult != Result::OK)
				return aResult;
		}
		File aFile;
		if (!Find(Name(thePath), aFile))
			return Result::OK;

		HttpRequest aRequest;
		aRequest.mUrl = std::format("{}/{}?alt=media", GOOGLE_FILES_URL, UrlEscape(aFile.mId));
		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK)
			return aResult;
		if (aResponse.mStatus == 404)
		{
			mListed = false;	// deleted since the listing
			return Result::OK;
		}
		aResult = Check(aResponse, "downloading " + DescribePath(thePath));
		if (aResult != Result::OK)
			return aResult;

		theFound = true;
		theBody = std::move(aResponse.mBody);
		theVersion = aFile.mVersion;
		return Result::OK;
	}

	Result Put(const std::string& thePath, const std::string& theBody, Condition theCondition, const std::string& theVersion) override
	{
		std::string aName = Name(thePath);
		std::string aWhat = "uploading " + DescribePath(thePath);
		if (!mListed || theCondition != Condition::ANY)
		{
			Result aResult = List();	// a condition is checked against a fresh listing
			if (aResult != Result::OK)
				return aResult;
		}

		File anExisting;
		bool anExists = Find(aName, anExisting);
		switch (theCondition)
		{
		case Condition::ANY:
			if (anExists)
				return Update(aName, anExisting, theBody, aWhat);
			return Create(aName, theBody, aWhat, anExisting);

		case Condition::ABSENT:
		{
			if (anExists)
				return Fail(Result::CONFLICT, RACE_LOST);
			File aMine;
			Result aResult = Create(aName, theBody, aWhat, aMine);
			if (aResult != Result::OK)
				return aResult;
			// No create-only upload either: when another device created the same name meanwhile, the older file wins.
			aResult = List();
			if (aResult != Result::OK)
				return aResult;
			File aWinner;
			if (Find(aName, aWinner) && aWinner.mId != aMine.mId)
			{
				DeleteId(aMine.mId, "removing the losing manifest");
				mListed = false;
				return Fail(Result::CONFLICT, RACE_LOST);
			}
			return Result::OK;
		}

		case Condition::VERSION:
			// and no If-Match: compare the version of a fresh listing right before replacing
			if (!anExists || anExisting.mVersion != theVersion)
				return Fail(Result::CONFLICT, RACE_LOST);
			return Update(aName, anExisting, theBody, aWhat);
		}
		return Fail(Result::FAILED, aWhat + " failed");
	}

	Result Delete(const std::string& thePath) override
	{
		if (!mListed)
		{
			Result aResult = List();
			if (aResult != Result::OK)
				return aResult;
		}
		auto anItr = mFiles.find(Name(thePath));
		if (anItr == mFiles.end())
			return Result::OK;
		for (const File& aFile : anItr->second)
		{
			Result aResult = DeleteId(aFile.mId, "deleting " + DescribePath(thePath));
			if (aResult != Result::OK)
				return aResult;
		}
		mFiles.erase(anItr);
		return Result::OK;
	}
};

// Apps/<app name>/<folder>/ in the player's OneDrive, addressed by path, with eTags on the manifest.
class OneDriveStore : public OAuthStore
{
	bool	mFoldersReady = false;

	std::string ItemUrl(const std::string& thePath) const
	{
		return std::format("{}:/{}", GRAPH_APPROOT_URL, UrlEscapePath(Name(thePath)));
	}

	// Commits through an upload session, which is where Graph documents if-match and conflictBehavior.
	Result PutConditional(const std::string& thePath, const std::string& theBody, Condition theCondition, const std::string& theVersion)
	{
		std::string aWhat = "uploading " + DescribePath(thePath);
		std::string aSession = std::format("{{\"item\":{{\"@microsoft.graph.conflictBehavior\":\"{}\"}}}}", theCondition == Condition::ABSENT ? "fail" : "replace");
		HttpRequest aRequest;
		aRequest.mMethod = "POST";
		aRequest.mUrl = ItemUrl(thePath) + ":/createUploadSession";
		aRequest.mBody = &aSession;
		aRequest.mContentType = "application/json";
		if (theCondition == Condition::VERSION && !theVersion.empty())
			aRequest.mHeaders.push_back("If-Match: " + theVersion);

		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK)
			return aResult;
		if (aResponse.mStatus == 409 || aResponse.mStatus == 412)
			return Fail(Result::CONFLICT, RACE_LOST);
		aResult = Check(aResponse, aWhat);
		if (aResult != Result::OK)
			return aResult;

		Json::Value aJson;
		std::string anUploadUrl = Json::Parse(aResponse.mBody, aJson) ? aJson.Str("uploadUrl") : std::string();
		if (anUploadUrl.empty())
			return Fail(Result::FAILED, aWhat + " failed: OneDrive sent no upload address");

		// the upload address carries its own authorization: no access token here
		HttpRequest anUpload;
		anUpload.mMethod = "PUT";
		anUpload.mUrl = anUploadUrl;
		anUpload.mBody = &theBody;
		anUpload.mHeaders.push_back(std::format("Content-Range: bytes 0-{}/{}", theBody.size() - 1, theBody.size()));
		HttpResponse anUploaded = mHttp->Request(anUpload);
		if (anUploaded.mStatus == 409 || anUploaded.mStatus == 412)
			return Fail(Result::CONFLICT, RACE_LOST);
		return Check(anUploaded, aWhat);
	}

public:
	OneDriveStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport) :
		OAuthStore(theConfig, theStateDir, std::move(theTransport))
	{
	}

	std::string Id() const override { return Key("onedrive"); }

	Result Get(const std::string& thePath, bool& theFound, std::string& theBody, std::string& theVersion) override
	{
		theFound = false;
		std::string aWhat = "downloading " + DescribePath(thePath);
		HttpRequest aRequest;
		aRequest.mUrl = ItemUrl(thePath);
		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK || aResponse.mStatus == 404)
			return aResult;
		aResult = Check(aResponse, aWhat);
		if (aResult != Result::OK)
			return aResult;

		Json::Value aJson;
		if (!Json::Parse(aResponse.mBody, aJson))
			return Fail(Result::FAILED, aWhat + " failed: OneDrive sent something that could not be read");
		std::string aVersion = aJson.Str("eTag");
		std::string aDownloadUrl = aJson.Str("@microsoft.graph.downloadUrl");

		HttpResponse aContent;
		HttpRequest aDownload;
		aDownload.mFollowRedirects = true;
		if (!aDownloadUrl.empty())
		{
			// a short-lived address that needs no access token
			aDownload.mUrl = aDownloadUrl;
			aContent = mHttp->Request(aDownload);
		}
		else
		{
			aDownload.mUrl = ItemUrl(thePath) + ":/content";
			aResult = Call(aDownload, aContent);
			if (aResult != Result::OK)
				return aResult;
		}
		aResult = Check(aContent, aWhat);
		if (aResult != Result::OK)
			return aResult;

		theFound = true;
		theBody = std::move(aContent.mBody);
		theVersion = aVersion;
		return Result::OK;
	}

	Result Put(const std::string& thePath, const std::string& theBody, Condition theCondition, const std::string& theVersion) override
	{
		if (theCondition != Condition::ANY && !theBody.empty())
			return PutConditional(thePath, theBody, theCondition, theVersion);

		HttpRequest aRequest;
		aRequest.mMethod = "PUT";
		aRequest.mUrl = ItemUrl(thePath) + ":/content";
		aRequest.mBody = &theBody;
		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK)
			return aResult;
		return Check(aResponse, "uploading " + DescribePath(thePath));
	}

	Result Delete(const std::string& thePath) override
	{
		HttpRequest aRequest;
		aRequest.mMethod = "DELETE";
		aRequest.mUrl = ItemUrl(thePath);
		HttpResponse aResponse;
		Result aResult = Call(aRequest, aResponse);
		if (aResult != Result::OK || aResponse.mStatus == 404)
			return aResult;
		return Check(aResponse, "deleting " + DescribePath(thePath));
	}

	Result Prepare() override
	{
		if (mFoldersReady)
			return Result::OK;

		// <folder>/files/, one level at a time; 409 means it is already there
		std::vector<std::string> aSegments;
		std::string aPath = Name("files");
		for (size_t aStart = 0; aStart <= aPath.size();)
		{
			size_t aSlash = aPath.find('/', aStart);
			if (aSlash == std::string::npos)
				aSlash = aPath.size();
			if (aSlash > aStart)
				aSegments.push_back(aPath.substr(aStart, aSlash - aStart));
			aStart = aSlash + 1;
		}

		std::string aParent;
		for (const std::string& aSegment : aSegments)
		{
			std::string aBody = std::format("{{\"name\":{},\"folder\":{{}},\"@microsoft.graph.conflictBehavior\":\"fail\"}}", Json::Quote(aSegment));
			HttpRequest aRequest;
			aRequest.mMethod = "POST";
			aRequest.mUrl = aParent.empty() ? std::format("{}/children", GRAPH_APPROOT_URL) : std::format("{}:/{}:/children", GRAPH_APPROOT_URL, UrlEscapePath(aParent));
			aRequest.mBody = &aBody;
			aRequest.mContentType = "application/json";

			HttpResponse aResponse;
			Result aResult = Call(aRequest, aResponse);
			if (aResult != Result::OK)
				return aResult;
			if (aResponse.mStatus != 409)
			{
				aResult = Check(aResponse, "creating the cloud folder");
				if (aResult != Result::OK)
					return aResult;
			}
			aParent = aParent.empty() ? aSegment : aParent + "/" + aSegment;
		}
		mFoldersReady = true;
		return Result::OK;
	}
};

}

std::unique_ptr<Store> MakeGoogleDriveStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport)
{
	return std::make_unique<GoogleDriveStore>(theConfig, theStateDir, std::move(theTransport));
}

std::unique_ptr<Store> MakeOneDriveStore(const Config& theConfig, const fs::path& theStateDir, std::unique_ptr<Transport> theTransport)
{
	return std::make_unique<OneDriveStore>(theConfig, theStateDir, std::move(theTransport));
}

Result DeviceSignIn(Transport& theHttp, const Config& theConfig, const std::function<void(const DeviceCode&)>& theOnCode,
	const std::function<bool(int)>& theWait, Token& theToken, std::string& theError)
{
	theToken = Token();
	auto aFail = [&theError](Result theResult, std::string theText)
	{
		theError = std::move(theText);
		return theResult;
	};

	if (!IsOAuth(theConfig.mProvider))
		return aFail(Result::FAILED, "WebDAV has no sign-in");
	const char* aName = ProviderName(theConfig.mProvider);
	Client aClient = ClientFor(theConfig);
	if (aClient.mId.empty())
		return aFail(Result::FAILED, std::format("enter the client ID of your {} app first (see the README)", aName));

	HttpResponse aResponse = PostForm(theHttp, aClient.mDeviceUrl, Form({ { "client_id", aClient.mId }, { "scope", aClient.mScope } }));
	if (aResponse.mStatus == 0)
		return aFail(Result::OFFLINE, aResponse.mError);

	Json::Value aJson;
	Json::Parse(aResponse.mBody, aJson);
	DeviceCode aCode;
	aCode.mDeviceCode = aJson.Str("device_code");
	aCode.mUserCode = aJson.Str("user_code");
	aCode.mUrl = aJson.Str("verification_url");		// Google's name for it
	if (aCode.mUrl.empty())
		aCode.mUrl = aJson.Str("verification_uri");		// RFC 8628, Microsoft
	aCode.mInterval = static_cast<int>(std::clamp<int64_t>(aJson.Int("interval", 5), 1, 60));
	aCode.mExpiresIn = static_cast<int>(std::clamp<int64_t>(aJson.Int("expires_in", 900), 60, 3600));
	if (!IsSuccess(aResponse.mStatus) || aCode.mDeviceCode.empty() || aCode.mUserCode.empty() || aCode.mUrl.empty())
		return aFail(Result::FAILED, std::format("{} would not start the sign-in: {}", aName, ErrorMessage(aResponse)));
	theOnCode(aCode);

	int anInterval = aCode.mInterval;
	for (int aWaited = 0; aWaited < aCode.mExpiresIn;)
	{
		if (!theWait(anInterval))
			return aFail(Result::FAILED, "the sign-in was cancelled");
		aWaited += anInterval;

		HttpResponse aPoll = PostForm(theHttp, aClient.mTokenUrl, Form({ { "client_id", aClient.mId }, { "client_secret", aClient.mSecret },
			{ "device_code", aCode.mDeviceCode }, { "grant_type", "urn:ietf:params:oauth:grant-type:device_code" } }));
		if (aPoll.mStatus == 0)
			continue;	// a dropped connection while the player signs in: just ask again

		Json::Value aPollJson;
		Json::Parse(aPoll.mBody, aPollJson);
		std::string anAccess = aPollJson.Str("access_token");
		if (IsSuccess(aPoll.mStatus) && !anAccess.empty())
		{
			theToken.mRefresh = aPollJson.Str("refresh_token");
			if (theToken.mRefresh.empty())
				return aFail(Result::FAILED, std::format("{} did not let the game stay signed in", aName));
			FetchAccount(theHttp, theConfig.mProvider, anAccess, theToken);
			if (theToken.mAccount.empty())
				theToken.mAccount = "device-" + RandomId();		// unknown account: never reuse another sign-in's sync history
			return Result::OK;
		}

		std::string anError = aPollJson.Str("error");
		if (anError == "authorization_pending")
			continue;
		if (anError == "slow_down")
		{
			anInterval += 5;
			continue;
		}
		if (anError == "access_denied" || anError == "authorization_declined")
			return aFail(Result::FAILED, "the sign-in was declined");
		if (anError == "expired_token")
			break;
		return aFail(Result::FAILED, std::format("signing in to {} failed: {}", aName, ErrorMessage(aPoll)));
	}
	return aFail(Result::FAILED, "the code expired before the sign-in was finished, try again");
}

}

#endif
