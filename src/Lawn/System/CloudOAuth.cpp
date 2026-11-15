/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// Google Drive and OneDrive: the OAuth sign-in in the browser and the two stores (see CloudSync.h).

#ifdef PVZP_CLOUD_SAVE

#include "CloudSync.h"
#include "CloudJson.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <exception>
#include <initializer_list>
#include <optional>
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
constexpr const char* GOOGLE_AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char* GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";
constexpr const char* GOOGLE_SCOPE = "https://www.googleapis.com/auth/drive.appdata";
constexpr const char* GOOGLE_FILES_URL = "https://www.googleapis.com/drive/v3/files";
constexpr const char* GOOGLE_UPLOAD_URL = "https://www.googleapis.com/upload/drive/v3/files";
constexpr const char* GOOGLE_ABOUT_URL = "https://www.googleapis.com/drive/v3/about";

// Files.ReadWrite.AppFolder: only Apps/<app name> in the player's OneDrive; offline_access gives the refresh token
constexpr const char* MICROSOFT_AUTH_URL = "https://login.microsoftonline.com/common/oauth2/v2.0/authorize";
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
	const char*	mAuthUrl;
	const char*	mTokenUrl;
	const char*	mScope;
	const char*	mRedirectHost;		// where the browser comes back to, at the port the game listens on
};

Client ClientFor(const Config& theConfig)
{
	// Google takes any loopback address for a Desktop app client. Microsoft matches the address registered for the
	// app, http://localhost, ignoring only the port.
	if (theConfig.mProvider == Provider::GOOGLE_DRIVE)
		return Client{ GoogleClientId(theConfig), GoogleClientSecret(theConfig), GOOGLE_AUTH_URL, GOOGLE_TOKEN_URL, GOOGLE_SCOPE, "http://127.0.0.1" };
	return Client{ OneDriveClientId(theConfig), "", MICROSOFT_AUTH_URL, MICROSOFT_TOKEN_URL, MICROSOFT_SCOPE, "http://localhost" };
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
// The code of an OAuth error goes first, as Google's descriptions ("Bad Request", "Unauthorized") say little alone.
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
				std::string aCode = Shorten(anError->mString);
				std::string aDescription = Shorten(aJson.Str("error_description"));
				if (!aCode.empty() && !aDescription.empty() && aDescription.find(aCode) == std::string::npos)
					return std::format("{}: {}", aCode, aDescription);
				if (!aCode.empty() || !aDescription.empty())
					return aDescription.empty() ? aCode : aDescription;
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

// For the PKCE verifier and the state, which must not be guessable: from the system's generator. libstdc++ may take
// a CPU instruction (RDSEED or RDRAND) first where there is one, which some CPUs have got wrong, so it is asked for
// /dev/urandom by name. Elsewhere random_device draws from the system's generator already.
std::string RandomBytes(size_t theCount)
{
	std::optional<std::random_device> aDevice;
#if defined(__GLIBCXX__) && !defined(_WIN32)
	try
	{
		aDevice.emplace("/dev/urandom");
	}
	catch (const std::exception&)
	{
	}
#endif
	if (!aDevice)
		aDevice.emplace();
	std::string aBytes;
	while (aBytes.size() < theCount)
	{
		uint32_t aValue = (*aDevice)();
		for (int i = 0; i < 4 && aBytes.size() < theCount; i++, aValue >>= 8)
			aBytes += static_cast<char>(aValue & 0xff);
	}
	return aBytes;
}

// Whether theScopes, a list of scopes split by spaces as a token response has them, holds theScope.
bool HasScope(std::string_view theScopes, std::string_view theScope)
{
	while (!theScopes.empty())
	{
		size_t aSpace = theScopes.find(' ');
		if (theScopes.substr(0, aSpace) == theScope)
			return true;
		theScopes = aSpace == std::string_view::npos ? std::string_view() : theScopes.substr(aSpace + 1);
	}
	return false;
}

// Best effort: a stable id of the account (and a name to show) right after the sign-in.
void FetchAccount(Transport& theHttp, Provider theProvider, const std::string& theAccess, Token& theToken)
{
	HttpRequest aRequest;
	aRequest.mBearer = theAccess;
	if (theProvider == Provider::GOOGLE_DRIVE)
		aRequest.mUrl = std::format("{}?fields={}", GOOGLE_ABOUT_URL, UrlEscape("user(permissionId,emailAddress)"));
	else
		aRequest.mUrl = std::format("{}?$select=id,parentReference", GRAPH_APPROOT_URL);	// the "$" as OData documents it

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
	Token					mOnFile;		// what the token file held when this session last read or wrote it
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
			std::string aCode = aParsed ? aJson.Str("error") : std::string();
			bool aGoogle = mConfig.mProvider == Provider::GOOGLE_DRIVE;
			if (aCode == "invalid_grant")
				return Fail(Result::FAILED, std::format("the {} sign-in expired or was revoked, sign in again", ProviderName(mConfig.mProvider)));
			// a refresh token from another client, such as the "TVs and Limited Input devices" one of earlier versions
			if (aGoogle && aCode == "unauthorized_client")
				return Fail(Result::FAILED, "the Google Drive sign-in was made with another client ID: press Sign Out, then Sign In again");
			if (aGoogle && aCode == "invalid_client")
				return Fail(Result::FAILED, "Google did not accept the client ID and secret: check that the secret belongs to this client ID (see the README)");
			return Fail(Result::FAILED, std::format("signing in to {} failed: {}", ProviderName(mConfig.mProvider), ErrorMessage(aResponse)));
		}

		mAccess = anAccess;
		int64_t aLifetime = std::clamp<int64_t>(aJson.Int("expires_in", 3600), 60, 86400);
		mExpiry = Clock::now() + std::chrono::seconds(aLifetime - 30);

		// Microsoft hands out a new refresh token with every refresh (the old one keeps working for a while). A file
		// from before the client ID was kept learns it from the first refresh that this client got through with.
		// The file is replaced only while it still holds what this session found or put there, so never over a
		// sign-in made since or a sign-out; a save that failed is made with the next refresh.
		Token aToken = mToken;
		std::string aRefresh = aJson.Str("refresh_token");
		if (!aRefresh.empty())
			aToken.mRefresh = aRefresh;
		if (aToken.mClient.empty())
			aToken.mClient = aClient.mId;
		if ((aToken.mRefresh != mOnFile.mRefresh || aToken.mClient != mOnFile.mClient) &&
			ReplaceToken(mStateDir, mConfig.mProvider, mOnFile.mRefresh, aToken))
			mOnFile = aToken;
		mToken = aToken;
		return Result::OK;
	}

public:
	OAuthSession(const Config& theConfig, fs::path theStateDir, Transport& theHttp) :
		mConfig(theConfig), mStateDir(std::move(theStateDir)), mHttp(theHttp)
	{
		mSignedIn = LoadTokenFor(mStateDir, mConfig, mToken);
		mOnFile = mToken;
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

// The code of a Graph error, {"error": {"code": "nameAlreadyExists", ...}}, which Graph says to go by: a status such as
// 409 has more than one cause.
std::string GraphErrorCode(const HttpResponse& theResponse)
{
	Json::Value aJson;
	const Json::Value* anError = Json::Parse(theResponse.mBody, aJson) ? aJson.Find("error") : nullptr;
	return anError && anError->mType == Json::Value::Type::OBJECT ? anError->Str("code") : std::string();
}

// Another device got there first: a file of that name made since (conflictBehavior "fail"), or a version other than
// the one read (If-Match), which Graph may also give as a 409 with resourceModified ("changed since the caller last
// read it"). Any other 409 is something else, such as a parent folder that is not there.
bool LostRace(const HttpResponse& theResponse)
{
	if (theResponse.mStatus == 412)
		return true;
	if (theResponse.mStatus != 409)
		return false;
	std::string aCode = GraphErrorCode(theResponse);
	return aCode == "nameAlreadyExists" || aCode == "resourceModified";
}

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
		if (LostRace(aResponse))
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
		if (anUploaded.mStatus == 200 || anUploaded.mStatus == 201)
			return Result::OK;

		// Otherwise Graph keeps the session, and what was uploaded to it, until the session expires: cancel it. But not
		// after no answer at all (offline, or stopped), when the cancel would most likely get none either.
		if (anUploaded.mStatus != 0)
		{
			HttpRequest aCancel;
			aCancel.mMethod = "DELETE";
			aCancel.mUrl = anUploadUrl;
			mHttp->Request(aCancel);
		}
		if (LostRace(anUploaded))
			return Fail(Result::CONFLICT, RACE_LOST);
		if (IsSuccess(anUploaded.mStatus))		// 202: it waits for more of a file that was sent whole
			return Fail(Result::FAILED, aWhat + " failed: OneDrive did not take the whole file");
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
		// An upload session cannot take an empty file. Only the manifest is written with a condition, and it is never empty.
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

		// <folder>/files/, one level at a time; a 409 with nameAlreadyExists means it is already there
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
			if (aResponse.mStatus != 409 || GraphErrorCode(aResponse) != "nameAlreadyExists")
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

std::string Sha256(std::string_view theData)
{
	static constexpr uint32_t K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	uint32_t aState[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	// the message, a one bit, zeros up to 56 bytes into the last block, and the length in bits
	std::string aMessage(theData);
	uint64_t aBits = static_cast<uint64_t>(theData.size()) * 8;
	aMessage += static_cast<char>(0x80);
	while (aMessage.size() % 64 != 56)
		aMessage += '\0';
	for (int i = 7; i >= 0; i--)
		aMessage += static_cast<char>((aBits >> (i * 8)) & 0xff);

	auto aRotate = [](uint32_t theValue, int theCount) { return (theValue >> theCount) | (theValue << (32 - theCount)); };
	auto aByte = [&aMessage](size_t theAt) { return static_cast<uint32_t>(static_cast<uint8_t>(aMessage[theAt])); };
	for (size_t aBlock = 0; aBlock < aMessage.size(); aBlock += 64)
	{
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
		{
			size_t anAt = aBlock + static_cast<size_t>(i) * 4;
			w[i] = (aByte(anAt) << 24) | (aByte(anAt + 1) << 16) | (aByte(anAt + 2) << 8) | aByte(anAt + 3);
		}
		for (int i = 16; i < 64; i++)
		{
			uint32_t s0 = aRotate(w[i - 15], 7) ^ aRotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = aRotate(w[i - 2], 17) ^ aRotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		uint32_t a = aState[0], b = aState[1], c = aState[2], d = aState[3], e = aState[4], f = aState[5], g = aState[6], h = aState[7];
		for (int i = 0; i < 64; i++)
		{
			uint32_t t1 = h + (aRotate(e, 6) ^ aRotate(e, 11) ^ aRotate(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
			uint32_t t2 = (aRotate(a, 2) ^ aRotate(a, 13) ^ aRotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		aState[0] += a;
		aState[1] += b;
		aState[2] += c;
		aState[3] += d;
		aState[4] += e;
		aState[5] += f;
		aState[6] += g;
		aState[7] += h;
	}

	std::string aDigest;
	for (uint32_t aWord : aState)
		for (int i = 3; i >= 0; i--)
			aDigest += static_cast<char>((aWord >> (i * 8)) & 0xff);
	return aDigest;
}

std::string Base64Url(std::string_view theData)
{
	static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	auto aByte = [&theData](size_t theAt) { return static_cast<uint32_t>(static_cast<uint8_t>(theData[theAt])); };

	std::string aText;
	size_t i = 0;
	for (; i + 2 < theData.size(); i += 3)
	{
		uint32_t aGroup = (aByte(i) << 16) | (aByte(i + 1) << 8) | aByte(i + 2);
		aText += ALPHABET[aGroup >> 18];
		aText += ALPHABET[(aGroup >> 12) & 63];
		aText += ALPHABET[(aGroup >> 6) & 63];
		aText += ALPHABET[aGroup & 63];
	}
	size_t aLeft = theData.size() - i;
	if (aLeft > 0)
	{
		uint32_t aGroup = (aByte(i) << 16) | (aLeft > 1 ? aByte(i + 1) << 8 : 0);
		aText += ALPHABET[aGroup >> 18];
		aText += ALPHABET[(aGroup >> 12) & 63];
		if (aLeft > 1)
			aText += ALPHABET[(aGroup >> 6) & 63];
	}
	return aText;
}

BrowserSignIn::BrowserSignIn(const Config& theConfig, std::unique_ptr<RedirectListener> theListener) :
	mConfig(theConfig), mListener(std::move(theListener))
{
}

BrowserSignIn::~BrowserSignIn() = default;

Result BrowserSignIn::Start(std::string& thePage, std::string& theError)
{
	thePage.clear();
	if (!IsOAuth(mConfig.mProvider))
	{
		theError = "WebDAV has no sign-in";
		return Result::FAILED;
	}
	const char* aName = ProviderName(mConfig.mProvider);
	Client aClient = ClientFor(mConfig);
	if (aClient.mId.empty())
	{
		theError = std::format("enter the client ID of your {} app first (see the README)", aName);
		return Result::FAILED;
	}
	if (mConfig.mProvider == Provider::GOOGLE_DRIVE && aClient.mSecret.empty())
	{
		// a secret typed in goes with a client ID typed in, never with the one built in
		theError = mConfig.mGoogleClientId.empty() ?
			"this build has a Google client ID but no client secret: enter the client ID and secret of your own Google app (see the README)" :
			"enter the client secret of your Google app first (see the README)";
		return Result::FAILED;
	}

	if (!mListener)
		mListener = MakeLoopbackListener();
	if (!mListener->Open(theError))
		return Result::FAILED;

	mVerifier = Base64Url(RandomBytes(32));		// 43 characters, as RFC 7636 asks for at least
	mState = Base64Url(RandomBytes(16));
	mRedirect = std::format("{}:{}", aClient.mRedirectHost, mListener->Port());
	std::string aChallenge = Base64Url(Sha256(mVerifier));
	if (mConfig.mProvider == Provider::GOOGLE_DRIVE)
	{
		// offline and consent: a refresh token every time, also for an account that allowed the game before
		thePage = std::format("{}?{}", aClient.mAuthUrl, Form({ { "client_id", aClient.mId }, { "redirect_uri", mRedirect },
			{ "response_type", "code" }, { "scope", aClient.mScope }, { "code_challenge", aChallenge }, { "code_challenge_method", "S256" },
			{ "state", mState }, { "access_type", "offline" }, { "prompt", "consent select_account" } }));
	}
	else
	{
		thePage = std::format("{}?{}", aClient.mAuthUrl, Form({ { "client_id", aClient.mId }, { "response_type", "code" },
			{ "redirect_uri", mRedirect }, { "response_mode", "query" }, { "scope", aClient.mScope }, { "state", mState },
			{ "code_challenge", aChallenge }, { "code_challenge_method", "S256" }, { "prompt", "select_account" } }));
	}
	return Result::OK;
}

Result BrowserSignIn::Finish(Transport& theHttp, const std::function<bool()>& theKeepWaiting, const std::function<void()>& theOnAnswer,
	const std::function<bool(const Token&)>& theOnSignedIn, Token& theToken, std::string& theError, std::chrono::seconds theTimeLimit)
{
	theToken = Token();
	theError.clear();
	if (!mListener || mState.empty())
	{
		theError = "the sign-in was not started";
		return Result::FAILED;
	}

	// used up by this sign-in, whatever comes of it: going away, it tells a browser still waiting that it failed
	std::unique_ptr<RedirectListener> aListener = std::move(mListener);
	const char* aName = ProviderName(mConfig.mProvider);
	const char* aStorage = mConfig.mProvider == Provider::GOOGLE_DRIVE ? "Google Drive" : "OneDrive";
	Clock::time_point aDeadline = Clock::now() + theTimeLimit;
	bool aTimedOut = false;
	Query aQuery;
	bool anAnswered = aListener->Wait(mState, aQuery, [&]()
	{
		if (!theKeepWaiting())
			return false;
		aTimedOut = Clock::now() >= aDeadline;
		return !aTimedOut;
	}, theError);
	if (!anAnswered)
	{
		// A client ID or redirect URI the provider does not know is shown on its page only (RFC 6749 section
		// 4.1.2.1): the browser never comes back then.
		if (theError.empty())
			theError = aTimedOut ? "the browser did not come back in time; if the sign-in page showed an error, check the client settings (see the README)" :
				"the sign-in was cancelled";
		return Result::FAILED;
	}
	if (theOnAnswer)
		theOnAnswer();

	if (aQuery.contains("error"))
	{
		const std::string& anError = aQuery["error"];
		if (anError == "access_denied")
		{
			aListener->Reply(RedirectListener::Outcome::DENIED);
			theError = std::format("the game was not given access to your {}", aStorage);
		}
		else
			theError = std::format("{} turned down the sign-in: {}", aName, Shorten(aQuery["error_description"].empty() ? anError : aQuery["error_description"]));
		return Result::FAILED;
	}
	if (aQuery["code"].empty())
	{
		theError = std::format("{} sent the browser back without a sign-in code", aName);
		return Result::FAILED;
	}

	// Microsoft's own example sends the scope again; Google takes it from the code alone
	Client aClient = ClientFor(mConfig);
	HttpResponse aResponse = PostForm(theHttp, aClient.mTokenUrl, Form({ { "client_id", aClient.mId }, { "client_secret", aClient.mSecret },
		{ "code", aQuery["code"] }, { "code_verifier", mVerifier }, { "grant_type", "authorization_code" }, { "redirect_uri", mRedirect },
		{ "scope", mConfig.mProvider == Provider::ONEDRIVE ? aClient.mScope : "" } }));
	if (aResponse.mStatus == 0)
	{
		theError = aResponse.mError;
		return Result::OFFLINE;
	}

	Json::Value aJson;
	bool aParsed = Json::Parse(aResponse.mBody, aJson);
	std::string anAccess = aParsed ? aJson.Str("access_token") : std::string();
	if (!IsSuccess(aResponse.mStatus) || anAccess.empty())
	{
		// the browser came back, so Google knows the client ID: it is the secret that does not go with it
		if (mConfig.mProvider == Provider::GOOGLE_DRIVE && aParsed && aJson.Str("error") == "invalid_client")
			theError = "Google did not accept the client secret: enter the secret of this client ID (see the README)";
		else
			theError = std::format("signing in to {} failed: {}", aName, ErrorMessage(aResponse));
		return Result::FAILED;
	}
	// Google lets the player untick the box for Drive and still sign in. No scope in the answer means all of
	// them (RFC 6749 section 5.1).
	if (mConfig.mProvider == Provider::GOOGLE_DRIVE && aJson.Find("scope") && !HasScope(aJson.Str("scope"), GOOGLE_SCOPE))
	{
		aListener->Reply(RedirectListener::Outcome::DENIED);
		theError = "the game was not given access to your Google Drive: tick the box for it on the Google page when you sign in again";
		return Result::FAILED;
	}
	theToken.mRefresh = aJson.Str("refresh_token");
	if (theToken.mRefresh.empty())
	{
		theError = std::format("{} did not let the game stay signed in", aName);
		return Result::FAILED;
	}
	theToken.mClient = aClient.mId;

	FetchAccount(theHttp, mConfig.mProvider, anAccess, theToken);
	if (theToken.mAccount.empty())
		theToken.mAccount = "device-" + RandomId();		// unknown account: never reuse another sign-in's sync history
	if (!theKeepWaiting())
	{
		// stopped by the player while it was fetched: the browser must not say it is done
		theToken = Token();
		theError = "the sign-in was cancelled";
		return Result::FAILED;
	}
	// kept before the browser says that it is done, so a game closed right after that still has it
	if (theOnSignedIn && !theOnSignedIn(theToken))
	{
		theToken = Token();
		theError = "the sign-in could not be saved";
		return Result::FAILED;
	}
	aListener->Reply(RedirectListener::Outcome::DONE);
	return Result::OK;
}

}

#endif
