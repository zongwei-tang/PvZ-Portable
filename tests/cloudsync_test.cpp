/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// Offline tests of the cloud save protocol: the sync engine against in-memory WebDAV, Google Drive and OneDrive
// servers that answer the way the real ones are documented to. Nothing here touches the network.
//
//   cmake -B build -DCLOUD_SAVE_TESTS=ON && cmake --build build --target cloudsync-test && build/cloudsync-test

#include "CloudSync.h"
#include "CloudJson.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace CloudSync;
namespace fs = std::filesystem;

namespace
{

// --- checks ---

int gChecks = 0;
int gFailures = 0;
std::string gTest;

std::string Show(const std::string& theText) { return "\"" + theText + "\""; }
std::string Show(const char* theText) { return Show(std::string(theText)); }
std::string Show(Result theResult) { return ResultName(theResult); }
std::string Show(Action theAction)
{
	switch (theAction)
	{
	case Action::NONE:		return "NONE";
	case Action::PUSH:		return "PUSH";
	case Action::PULL:		return "PULL";
	case Action::CONFLICT:	return "CONFLICT";
	}
	return "?";
}
template <typename T> requires std::is_arithmetic_v<T>
std::string Show(T theValue)
{
	if constexpr (std::is_same_v<T, bool>)
		return theValue ? "true" : "false";
	else
		return std::to_string(theValue);
}

void Report(bool theOk, int theLine, const std::string& theWhat)
{
	gChecks++;
	if (!theOk)
	{
		gFailures++;
		std::cerr << std::format("FAILED {} (line {}): {}\n", gTest, theLine, theWhat);
	}
}

#define CHECK(theCondition) Report((theCondition), __LINE__, #theCondition)
#define CHECK_EQ(theA, theB) \
	do { auto&& anA_ = (theA); auto&& aB_ = (theB); \
		 Report(anA_ == aB_, __LINE__, std::format("{} == {}, got {} and {}", #theA, #theB, Show(anA_), Show(aB_))); } while (false)

// --- HTTP helpers ---

std::string UrlDecode(std::string_view theText)
{
	std::string anOut;
	for (size_t i = 0; i < theText.size(); i++)
	{
		if (theText[i] == '%' && i + 2 < theText.size())
		{
			anOut += static_cast<char>(std::stoi(std::string(theText.substr(i + 1, 2)), nullptr, 16));
			i += 2;
		}
		else if (theText[i] == '+')
			anOut += ' ';
		else
			anOut += theText[i];
	}
	return anOut;
}

std::map<std::string, std::string> ParsePairs(std::string_view theText)
{
	std::map<std::string, std::string> aPairs;
	while (!theText.empty())
	{
		size_t anAmp = theText.find('&');
		std::string_view aPair = theText.substr(0, anAmp);
		size_t anEquals = aPair.find('=');
		aPairs[UrlDecode(aPair.substr(0, anEquals))] = anEquals == std::string_view::npos ? "" : UrlDecode(aPair.substr(anEquals + 1));
		theText = anAmp == std::string_view::npos ? std::string_view() : theText.substr(anAmp + 1);
	}
	return aPairs;
}

struct Url
{
	std::string							mPath;		// all before the '?'
	std::map<std::string, std::string>	mQuery;
};

Url ParseUrl(const std::string& theUrl)
{
	Url aUrl;
	size_t aQuestion = theUrl.find('?');
	aUrl.mPath = theUrl.substr(0, aQuestion);
	if (aQuestion != std::string::npos)
		aUrl.mQuery = ParsePairs(std::string_view(theUrl).substr(aQuestion + 1));
	return aUrl;
}

std::string Header(const HttpRequest& theRequest, std::string_view theName)
{
	for (const std::string& aHeader : theRequest.mHeaders)
	{
		size_t aColon = aHeader.find(':');
		if (aColon == theName.size() && std::equal(theName.begin(), theName.end(), aHeader.begin(),
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
		{
			size_t aStart = aHeader.find_first_not_of(' ', aColon + 1);
			return aStart == std::string::npos ? "" : aHeader.substr(aStart);
		}
	}
	return "";
}

std::string Body(const HttpRequest& theRequest)
{
	return theRequest.mBody ? *theRequest.mBody : std::string();
}

HttpResponse Answer(long theStatus, std::string theBody = "", std::string theETag = "")
{
	HttpResponse aResponse;
	aResponse.mStatus = theStatus;
	aResponse.mBody = std::move(theBody);
	aResponse.mETag = std::move(theETag);
	return aResponse;
}

// An in-memory server. A hook runs once, right before the first request it matches is answered, so a test can let
// another device act in the middle of a sync.
struct Server
{
	bool										mOffline = false;
	std::vector<std::string>					mLog;
	std::function<bool(const HttpRequest&)>		mHook;

	virtual ~Server() = default;
	virtual HttpResponse Handle(const HttpRequest& theRequest) = 0;

	HttpResponse Request(const HttpRequest& theRequest)
	{
		if (mHook)
		{
			auto aHook = std::move(mHook);
			mHook = nullptr;	// the requests the hook makes itself must not run it again
			if (!aHook(theRequest) && !mHook)
				mHook = std::move(aHook);
		}
		mLog.push_back(theRequest.mMethod + " " + theRequest.mUrl);
		if (mOffline)
		{
			HttpResponse aResponse;
			aResponse.mError = "Could not resolve host";
			return aResponse;
		}
		return Handle(theRequest);
	}

	void Before(std::function<bool(const HttpRequest&)> theMatch, std::function<void()> theAction)
	{
		mHook = [theMatch = std::move(theMatch), theAction = std::move(theAction)](const HttpRequest& theRequest)
		{
			if (!theMatch(theRequest))
				return false;
			theAction();
			return true;
		};
	}

	int Count(std::string_view theText) const
	{
		int aCount = 0;
		for (const std::string& anEntry : mLog)
			aCount += anEntry.find(theText) != std::string::npos;
		return aCount;
	}
};

class Link : public Transport
{
	Server&		mServer;

public:
	explicit Link(Server& theServer) : mServer(theServer) {}
	HttpResponse Request(const HttpRequest& theRequest) override { return mServer.Request(theRequest); }
};

std::unique_ptr<Transport> LinkTo(Server& theServer)
{
	return std::make_unique<Link>(theServer);
}

// --- WebDAV ---

struct DavServer : Server
{
	struct File
	{
		std::string	mBody;
		int			mTag = 0;
	};

	static constexpr std::string_view HOST = "https://dav.example";

	std::map<std::string, File>	mFiles;		// by path, as sent
	std::set<std::string>		mFolders;
	std::string					mUser = "alice";
	std::string					mPassword = "secret";
	bool						mConditional = true;	// honours If-Match and If-None-Match
	bool						mWeakETags = false;
	int							mNextTag = 1;
	std::vector<std::string>	mIfMatches;		// If-Match headers of the PUTs, as received

	std::string ETag(const File& theFile) const
	{
		return std::format("{}\"t{}\"", mWeakETags ? "W/" : "", theFile.mTag);
	}

	HttpResponse Handle(const HttpRequest& theRequest) override
	{
		if (theRequest.mUsername != mUser || theRequest.mPassword != mPassword)
			return Answer(401);
		if (!theRequest.mUrl.starts_with(HOST))
			return Answer(404);
		std::string aPath = theRequest.mUrl.substr(HOST.size());
		auto anItr = mFiles.find(aPath);

		if (theRequest.mMethod == "GET")
			return anItr == mFiles.end() ? Answer(404) : Answer(200, anItr->second.mBody, ETag(anItr->second));
		if (theRequest.mMethod == "PUT")
		{
			std::string anIfMatch = Header(theRequest, "If-Match");
			std::string anIfNoneMatch = Header(theRequest, "If-None-Match");
			mIfMatches.push_back(anIfMatch);
			if (mConditional)
			{
				if (anIfNoneMatch == "*" && anItr != mFiles.end())
					return Answer(412);
				// a weak ETag never matches If-Match (RFC 9110, strong comparison)
				if (!anIfMatch.empty() && (anItr == mFiles.end() || anIfMatch.starts_with("W/") || ETag(anItr->second) != anIfMatch))
					return Answer(412);
			}
			bool aNew = anItr == mFiles.end();
			mFiles[aPath] = File{ Body(theRequest), mNextTag++ };
			return Answer(aNew ? 201 : 204);
		}
		if (theRequest.mMethod == "DELETE")
		{
			if (anItr == mFiles.end())
				return Answer(404);
			mFiles.erase(anItr);
			return Answer(204);
		}
		if (theRequest.mMethod == "MKCOL")
			return mFolders.insert(aPath).second ? Answer(201) : Answer(405);
		return Answer(405);
	}

	static std::string Root() { return "/remote.php/dav/files/alice/PvZPortable/"; }

	const File* Manifest() const
	{
		auto anItr = mFiles.find(Root() + "manifest.txt");
		return anItr == mFiles.end() ? nullptr : &anItr->second;
	}

	int Blobs() const
	{
		int aCount = 0;
		for (const auto& [aPath, aFile] : mFiles)
			aCount += aPath.starts_with(Root() + "files/");
		return aCount;
	}

	// removes the blob of theName, as a damaged cloud folder would lack it
	bool Damage(const std::string& theName)
	{
		for (auto anItr = mFiles.begin(); anItr != mFiles.end(); ++anItr)
		{
			if (anItr->first.starts_with(Root() + "files/" + theName + "."))
			{
				mFiles.erase(anItr);
				return true;
			}
		}
		return false;
	}
};

Config DavConfig()
{
	Config aConfig;
	aConfig.mEnabled = true;
	aConfig.mProvider = Provider::WEBDAV;
	aConfig.mUrl = "https://dav.example/remote.php/dav/files/alice/";
	aConfig.mUsername = "alice";
	aConfig.mPassword = "secret";
	return aConfig;
}

// --- Google Drive ---

struct GoogleServer : Server
{
	struct File
	{
		std::string	mId;
		std::string	mName;
		std::string	mCreated;
		std::string	mBody;
		int64_t		mVersion = 1;
	};

	static constexpr std::string_view FILES = "https://www.googleapis.com/drive/v3/files";
	static constexpr std::string_view UPLOAD = "https://www.googleapis.com/upload/drive/v3/files";

	std::vector<File>			mFiles;
	int							mNextId = 1;
	int							mClock = 0;
	std::string					mClientId = "google-client";
	std::string					mSecret = "google-secret";
	std::set<std::string>		mAccessTokens;
	std::set<std::string>		mRefreshTokens;
	int							mSerial = 0;
	std::deque<std::string>		mPolls;		// answers to the device code polls: pending, slow_down, denied, expired, ok
	int							mRefreshes = 0;
	size_t						mPageSize = 1000;

	std::string FileJson(const File& theFile) const
	{
		return std::format(R"({{"id":"{}","name":{},"createdTime":"{}","version":"{}","size":"{}"}})",
			theFile.mId, Json::Quote(theFile.mName), theFile.mCreated, theFile.mVersion, theFile.mBody.size());
	}

	File* FindId(const std::string& theId)
	{
		for (File& aFile : mFiles)
			if (aFile.mId == theId)
				return &aFile;
		return nullptr;
	}

	std::string IssueRefresh()
	{
		std::string aToken = std::format("g-refresh-{}", ++mSerial);
		mRefreshTokens.insert(aToken);
		return aToken;
	}

	HttpResponse Token(const HttpRequest& theRequest)
	{
		auto aForm = ParsePairs(Body(theRequest));
		if (aForm["client_id"] != mClientId || aForm["client_secret"] != mSecret)
			return Answer(401, R"({"error":"invalid_client","error_description":"Unauthorized"})");

		if (aForm["grant_type"] == "urn:ietf:params:oauth:grant-type:device_code")
		{
			if (aForm["device_code"] != "g-device")
				return Answer(400, R"({"error":"invalid_grant","error_description":"Malformed auth code."})");
			std::string anAnswer = mPolls.empty() ? "pending" : mPolls.front();
			if (!mPolls.empty())
				mPolls.pop_front();
			if (anAnswer == "pending")
				return Answer(428, R"({"error":"authorization_pending","error_description":"Precondition Required"})");
			if (anAnswer == "slow_down")
				return Answer(403, R"({"error":"slow_down","error_description":"Forbidden"})");
			if (anAnswer == "denied")
				return Answer(403, R"({"error":"access_denied","error_description":"Forbidden"})");
			if (anAnswer == "expired")
				return Answer(400, R"({"error":"expired_token","error_description":"Bad Request"})");
			std::string anAccess = std::format("g-access-{}", ++mSerial);
			mAccessTokens.insert(anAccess);
			return Answer(200, std::format(R"({{"access_token":"{}","expires_in":3599,"refresh_token":"{}","scope":"https://www.googleapis.com/auth/drive.appdata","token_type":"Bearer"}})",
				anAccess, IssueRefresh()));
		}
		if (aForm["grant_type"] == "refresh_token")
		{
			if (!mRefreshTokens.contains(aForm["refresh_token"]))
				return Answer(400, R"({"error":"invalid_grant","error_description":"Token has been expired or revoked."})");
			mRefreshes++;
			std::string anAccess = std::format("g-access-{}", ++mSerial);
			mAccessTokens.insert(anAccess);
			return Answer(200, std::format(R"({{"access_token":"{}","expires_in":3599,"scope":"https://www.googleapis.com/auth/drive.appdata","token_type":"Bearer"}})", anAccess));
		}
		return Answer(400, R"({"error":"unsupported_grant_type"})");
	}

	HttpResponse List(Url theUrl)
	{
		if (theUrl.mQuery["spaces"] != "appDataFolder" || theUrl.mQuery["fields"].find("files(id,name,createdTime,version,size)") == std::string::npos)
			return Answer(400, R"({"error":{"code":400,"message":"Invalid list request"}})");
		// newest first, so the client has to sort by creation time itself
		std::vector<const File*> aAll;
		for (auto anItr = mFiles.rbegin(); anItr != mFiles.rend(); ++anItr)
			aAll.push_back(&*anItr);
		size_t aStart = theUrl.mQuery.contains("pageToken") ? std::stoul(theUrl.mQuery.at("pageToken")) : 0;
		std::string aBody = "{";
		if (aStart + mPageSize < aAll.size())
			aBody += std::format(R"("nextPageToken":"{}",)", aStart + mPageSize);
		aBody += R"("files":[)";
		for (size_t i = aStart; i < aAll.size() && i < aStart + mPageSize; i++)
			aBody += (i > aStart ? "," : "") + FileJson(*aAll[i]);
		aBody += "]}";
		return Answer(200, aBody);
	}

	HttpResponse Create(const HttpRequest& theRequest)
	{
		std::string aType = theRequest.mContentType;
		size_t aBoundaryAt = aType.find("boundary=");
		if (!aType.starts_with("multipart/related") || aBoundaryAt == std::string::npos)
			return Answer(400, R"({"error":{"code":400,"message":"Expected a multipart upload"}})");
		std::string aDelimiter = "--" + aType.substr(aBoundaryAt + 9);
		std::string aBody = Body(theRequest);

		// --b CRLF headers CRLF CRLF metadata CRLF --b CRLF headers CRLF CRLF content CRLF --b-- CRLF
		std::vector<std::string> aParts;
		size_t aPos = aBody.find(aDelimiter);
		while (aPos != std::string::npos)
		{
			size_t aStart = aPos + aDelimiter.size();
			if (aBody.compare(aStart, 2, "--") == 0)
				break;
			size_t aNext = aBody.find("\r\n" + aDelimiter, aStart);
			if (aNext == std::string::npos)
				return Answer(400, R"({"error":{"code":400,"message":"Unterminated part"}})");
			std::string aPart = aBody.substr(aStart + 2, aNext - aStart - 2);
			size_t aBlank = aPart.find("\r\n\r\n");
			aParts.push_back(aBlank == std::string::npos ? "" : aPart.substr(aBlank + 4));
			aPos = aNext + 2;
		}

		Json::Value aMeta;
		if (aParts.size() != 2 || !Json::Parse(aParts[0], aMeta))
			return Answer(400, R"({"error":{"code":400,"message":"Bad multipart body"}})");
		const Json::Value* aParents = aMeta.Find("parents");
		if (!aParents || aParents->mArray.size() != 1 || aParents->mArray[0].mString != "appDataFolder")
			return Answer(403, R"({"error":{"code":403,"message":"The app may only write to appDataFolder"}})");

		mClock++;
		File aFile{ std::format("id-{}", mNextId++), aMeta.Str("name"), std::format("2026-11-14T10:00:{:02}.{:03}Z", mClock / 1000, mClock % 1000), aParts[1], 1 };
		mFiles.push_back(aFile);
		return Answer(200, FileJson(aFile));
	}

	HttpResponse Handle(const HttpRequest& theRequest) override
	{
		Url aUrl = ParseUrl(theRequest.mUrl);
		if (aUrl.mPath == "https://oauth2.googleapis.com/device/code")
		{
			auto aForm = ParsePairs(Body(theRequest));
			if (aForm["client_id"] != mClientId)
				return Answer(401, R"({"error":"invalid_client","error_description":"The OAuth client was not found."})");
			if (aForm["scope"] != "https://www.googleapis.com/auth/drive.appdata")
				return Answer(400, R"({"error":"invalid_scope"})");
			return Answer(200, R"({"device_code":"g-device","user_code":"GQVQ-JKEC","expires_in":1800,"interval":5,"verification_url":"https://www.google.com/device"})");
		}
		if (aUrl.mPath == "https://oauth2.googleapis.com/token")
			return Token(theRequest);

		if (!mAccessTokens.contains(theRequest.mBearer))
			return Answer(401, R"({"error":{"code":401,"message":"Request had invalid authentication credentials."}})");
		if (aUrl.mPath == "https://www.googleapis.com/drive/v3/about")
			return Answer(200, R"({"user":{"permissionId":"perm-123","emailAddress":"player@example.com"}})");
		if (aUrl.mPath == FILES && theRequest.mMethod == "GET")
			return List(aUrl);
		if (aUrl.mPath == UPLOAD && theRequest.mMethod == "POST" && aUrl.mQuery["uploadType"] == "multipart")
			return Create(theRequest);
		if (aUrl.mPath.starts_with(std::string(UPLOAD) + "/") && theRequest.mMethod == "PATCH" && aUrl.mQuery["uploadType"] == "media")
		{
			File* aFile = FindId(UrlDecode(aUrl.mPath.substr(UPLOAD.size() + 1)));
			if (aFile == nullptr)
				return Answer(404, R"({"error":{"code":404,"message":"File not found"}})");
			aFile->mBody = Body(theRequest);
			aFile->mVersion++;
			return Answer(200, FileJson(*aFile));
		}
		if (aUrl.mPath.starts_with(std::string(FILES) + "/"))
		{
			std::string anId = UrlDecode(aUrl.mPath.substr(FILES.size() + 1));
			File* aFile = FindId(anId);
			if (theRequest.mMethod == "GET" && aUrl.mQuery["alt"] == "media")
				return aFile ? Answer(200, aFile->mBody) : Answer(404, R"({"error":{"code":404,"message":"File not found"}})");
			if (theRequest.mMethod == "DELETE")
			{
				if (aFile == nullptr)
					return Answer(404, R"({"error":{"code":404,"message":"File not found"}})");
				std::erase_if(mFiles, [&anId](const File& theFile) { return theFile.mId == anId; });
				return Answer(204);
			}
		}
		return Answer(400, R"({"error":{"code":400,"message":"Unexpected request"}})");
	}

	std::vector<const File*> Named(const std::string& theName) const
	{
		std::vector<const File*> aFiles;
		for (const File& aFile : mFiles)
			if (aFile.mName == theName)
				aFiles.push_back(&aFile);
		return aFiles;
	}

	File* Manifest()
	{
		for (File& aFile : mFiles)
			if (aFile.mName == "PvZPortable/manifest.txt")
				return &aFile;
		return nullptr;
	}
};

Config GoogleConfig()
{
	Config aConfig;
	aConfig.mEnabled = true;
	aConfig.mProvider = Provider::GOOGLE_DRIVE;
	aConfig.mGoogleClientId = "google-client";
	aConfig.mGoogleClientSecret = "google-secret";
	return aConfig;
}

// --- OneDrive ---

struct OneDriveServer : Server
{
	struct Item
	{
		bool		mFolder = false;
		std::string	mBody;
		int			mTag = 0;
	};

	struct Session
	{
		std::string	mPath;
		std::string	mIfMatch;
		bool		mFail = false;
	};

	static constexpr std::string_view APPROOT = "https://graph.microsoft.com/v1.0/me/drive/special/approot";

	std::map<std::string, Item>			mItems;			// by path below the app folder
	std::map<std::string, Session>		mSessions;		// by upload address
	std::map<std::string, std::string>	mDownloads;		// download address -> path
	std::string							mClientId = "onedrive-client";
	std::set<std::string>				mAccessTokens;
	std::set<std::string>				mRefreshTokens;	// each refresh replaces the token it was made with
	int									mSerial = 0;
	std::deque<std::string>				mPolls;
	int									mRefreshes = 0;
	bool								mConflictAtUpload = true;	// conflictBehavior "fail" is checked when the upload completes
	int									mNextTag = 1;
	int									mTokensOnPreAuthorized = 0;	// requests to upload or download addresses that carried a token

	std::string ETag(const Item& theItem) const
	{
		return std::format("\"{{8B4C1D2E-0000-4000-8000-000000000001}},{}\"", theItem.mTag);
	}

	std::string IssueRefresh()
	{
		std::string aToken = std::format("m-refresh-{}", ++mSerial);
		mRefreshTokens.insert(aToken);
		return aToken;
	}

	HttpResponse Token(const HttpRequest& theRequest)
	{
		auto aForm = ParsePairs(Body(theRequest));
		if (aForm["client_id"] != mClientId)
			return Answer(400, R"({"error":"unauthorized_client","error_description":"AADSTS700016: Application not found."})");
		if (aForm.contains("client_secret"))
			return Answer(400, R"({"error":"invalid_request","error_description":"AADSTS90023: Public clients can't send a client secret."})");

		if (aForm["grant_type"] == "urn:ietf:params:oauth:grant-type:device_code")
		{
			std::string anAnswer = mPolls.empty() ? "pending" : mPolls.front();
			if (!mPolls.empty())
				mPolls.pop_front();
			if (anAnswer == "pending")
				return Answer(400, "{\"error\":\"authorization_pending\",\"error_description\":\"AADSTS70016: OAuth 2.0 device flow error. Authorization is pending.\\r\\nTrace ID: 1\\r\\nCorrelation ID: 2\"}");
			if (anAnswer == "denied")
				return Answer(400, R"({"error":"authorization_declined","error_description":"AADSTS70000: The user declined."})");
			if (anAnswer == "expired")
				return Answer(400, R"({"error":"expired_token","error_description":"AADSTS70019: Verification code expired."})");
			std::string anAccess = std::format("m-access-{}", ++mSerial);
			mAccessTokens.insert(anAccess);
			return Answer(200, std::format(R"({{"token_type":"Bearer","scope":"Files.ReadWrite.AppFolder","expires_in":3600,"access_token":"{}","refresh_token":"{}"}})", anAccess, IssueRefresh()));
		}
		if (aForm["grant_type"] == "refresh_token")
		{
			auto anItr = mRefreshTokens.find(aForm["refresh_token"]);
			if (anItr == mRefreshTokens.end())
				return Answer(400, R"({"error":"invalid_grant","error_description":"AADSTS70000: The provided grant has expired."})");
			mRefreshTokens.erase(anItr);
			mRefreshes++;
			std::string anAccess = std::format("m-access-{}", ++mSerial);
			mAccessTokens.insert(anAccess);
			return Answer(200, std::format(R"({{"token_type":"Bearer","scope":"Files.ReadWrite.AppFolder","expires_in":3600,"access_token":"{}","refresh_token":"{}"}})", anAccess, IssueRefresh()));
		}
		return Answer(400, R"({"error":"unsupported_grant_type"})");
	}

	HttpResponse Metadata(const std::string& thePath, const Item& theItem)
	{
		std::string aDownload = std::format("https://dl.example/{}", mDownloads.size() + 1);
		mDownloads[aDownload] = thePath;
		return Answer(200, std::format(R"({{"id":"item-{}","name":{},"eTag":{},"size":{},"file":{{}},"@microsoft.graph.downloadUrl":"{}"}})",
			theItem.mTag, Json::Quote(thePath.substr(thePath.rfind('/') + 1)), Json::Quote(ETag(theItem)), theItem.mBody.size(), aDownload));
	}

	HttpResponse CreateFolder(const std::string& theParent, const std::string& theBody)
	{
		Json::Value aJson;
		if (!Json::Parse(theBody, aJson) || !aJson.Find("folder") || aJson.Str("@microsoft.graph.conflictBehavior") != "fail")
			return Answer(400, R"({"error":{"code":"invalidRequest","message":"Bad folder request"}})");
		if (!theParent.empty() && !mItems.contains(theParent))
			return Answer(404, R"({"error":{"code":"itemNotFound","message":"The parent does not exist"}})");
		std::string aPath = theParent.empty() ? aJson.Str("name") : theParent + "/" + aJson.Str("name");
		if (mItems.contains(aPath))
			return Answer(409, R"({"error":{"code":"nameAlreadyExists","message":"An item with the same name already exists under the parent"}})");
		mItems[aPath] = Item{ true, "", mNextTag++ };
		return Answer(201, std::format(R"({{"id":"folder-{}","name":{},"folder":{{"childCount":0}}}})", mNextTag, Json::Quote(aJson.Str("name"))));
	}

	HttpResponse Complete(const HttpRequest& theRequest)
	{
		auto aSession = mSessions.find(theRequest.mUrl);
		if (aSession == mSessions.end())
			return Answer(404, R"({"error":{"code":"itemNotFound","message":"The upload session does not exist"}})");
		std::string aBody = Body(theRequest);
		if (theRequest.mMethod != "PUT" || Header(theRequest, "Content-Range") != std::format("bytes 0-{}/{}", aBody.size() - 1, aBody.size()))
			return Answer(400, R"({"error":{"code":"invalidRange","message":"Bad Content-Range"}})");

		Session aDone = aSession->second;
		mSessions.erase(aSession);
		auto anItem = mItems.find(aDone.mPath);
		if (aDone.mFail && anItem != mItems.end())
			return Answer(409, R"({"error":{"code":"nameAlreadyExists","message":"The specified item name already exists."}})");
		if (!aDone.mIfMatch.empty() && (anItem == mItems.end() || ETag(anItem->second) != aDone.mIfMatch))
			return Answer(412, R"({"error":{"code":"preconditionFailed","message":"ETag does not match current item's value"}})");
		mItems[aDone.mPath] = Item{ false, aBody, mNextTag++ };
		return Answer(201, std::format(R"({{"id":"item","eTag":{}}})", Json::Quote(ETag(mItems[aDone.mPath]))));
	}

	HttpResponse Handle(const HttpRequest& theRequest) override
	{
		Url aUrl = ParseUrl(theRequest.mUrl);
		if (aUrl.mPath == "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode")
		{
			auto aForm = ParsePairs(Body(theRequest));
			if (aForm["client_id"] != mClientId || aForm["scope"] != "Files.ReadWrite.AppFolder offline_access")
				return Answer(400, R"({"error":"invalid_request","error_description":"AADSTS900144: Bad request."})");
			return Answer(200, R"({"user_code":"ABCD2345","device_code":"m-device","verification_uri":"https://microsoft.com/devicelogin","expires_in":900,"interval":5,"message":"To sign in, use a web browser."})");
		}
		if (aUrl.mPath == "https://login.microsoftonline.com/common/oauth2/v2.0/token")
			return Token(theRequest);

		// addresses that carry their own authorization
		if (theRequest.mUrl.starts_with("https://dl.example/") || theRequest.mUrl.starts_with("https://up.example/"))
		{
			mTokensOnPreAuthorized += !theRequest.mBearer.empty();
			if (theRequest.mUrl.starts_with("https://up.example/"))
				return Complete(theRequest);
			auto aDownload = mDownloads.find(theRequest.mUrl);
			auto anItem = aDownload == mDownloads.end() ? mItems.end() : mItems.find(aDownload->second);
			return anItem == mItems.end() ? Answer(404) : Answer(200, anItem->second.mBody);
		}

		if (!mAccessTokens.contains(theRequest.mBearer))
			return Answer(401, R"({"error":{"code":"InvalidAuthenticationToken","message":"Access token has expired or is not yet valid."}})");
		if (!aUrl.mPath.starts_with(APPROOT))
			return Answer(400, R"({"error":{"code":"invalidRequest","message":"Outside the app folder"}})");

		std::string aRest = aUrl.mPath.substr(APPROOT.size());
		if (aRest.empty() && theRequest.mMethod == "GET")
			return Answer(200, R"({"id":"approot-id","name":"PvZ-Portable","parentReference":{"driveId":"drive-42","driveType":"personal"}})");
		if (aRest == "/children" && theRequest.mMethod == "POST")
			return CreateFolder("", Body(theRequest));
		if (!aRest.starts_with(":/"))
			return Answer(400, R"({"error":{"code":"invalidRequest","message":"Unexpected address"}})");

		aRest = aRest.substr(2);
		size_t anAction = aRest.find(":/");
		std::string aPath = UrlDecode(aRest.substr(0, anAction));
		std::string anActionName = anAction == std::string::npos ? "" : aRest.substr(anAction + 2);
		auto anItem = mItems.find(aPath);

		if (anActionName == "children" && theRequest.mMethod == "POST")
			return CreateFolder(aPath, Body(theRequest));
		if (anActionName == "content" && theRequest.mMethod == "PUT")
		{
			mItems[aPath] = Item{ false, Body(theRequest), mNextTag++ };
			return Answer(201, std::format(R"({{"id":"item","eTag":{}}})", Json::Quote(ETag(mItems[aPath]))));
		}
		if (anActionName == "content" && theRequest.mMethod == "GET")
			return anItem == mItems.end() ? Answer(404) : Answer(200, anItem->second.mBody);
		if (anActionName == "createUploadSession" && theRequest.mMethod == "POST")
		{
			Json::Value aJson;
			const Json::Value* anItemJson = Json::Parse(Body(theRequest), aJson) ? aJson.Find("item") : nullptr;
			std::string aBehavior = anItemJson ? anItemJson->Str("@microsoft.graph.conflictBehavior") : "";
			if (aBehavior != "fail" && aBehavior != "replace")
				return Answer(400, R"({"error":{"code":"invalidRequest","message":"Bad upload session request"}})");
			std::string anIfMatch = Header(theRequest, "If-Match");
			if (!anIfMatch.empty() && (anItem == mItems.end() || ETag(anItem->second) != anIfMatch))
				return Answer(412, R"({"error":{"code":"preconditionFailed","message":"ETag does not match current item's value"}})");
			if (aBehavior == "fail" && anItem != mItems.end() && !mConflictAtUpload)
				return Answer(409, R"({"error":{"code":"nameAlreadyExists","message":"The specified item name already exists."}})");
			std::string anUpload = std::format("https://up.example/session-{}", ++mSerial);
			mSessions[anUpload] = Session{ aPath, anIfMatch, aBehavior == "fail" };
			return Answer(200, std::format(R"({{"uploadUrl":"{}","expirationDateTime":"2026-11-15T10:00:00Z"}})", anUpload));
		}
		if (anActionName.empty() && theRequest.mMethod == "GET")
			return anItem == mItems.end() || anItem->second.mFolder ? Answer(404, R"({"error":{"code":"itemNotFound","message":"Item does not exist"}})") : Metadata(aPath, anItem->second);
		if (anActionName.empty() && theRequest.mMethod == "DELETE")
		{
			if (anItem == mItems.end())
				return Answer(404, R"({"error":{"code":"itemNotFound","message":"Item does not exist"}})");
			mItems.erase(anItem);
			return Answer(204);
		}
		return Answer(400, R"({"error":{"code":"invalidRequest","message":"Unexpected request"}})");
	}

	Item* Manifest()
	{
		auto anItr = mItems.find("PvZPortable/manifest.txt");
		return anItr == mItems.end() ? nullptr : &anItr->second;
	}
};

Config OneDriveConfig()
{
	Config aConfig;
	aConfig.mEnabled = true;
	aConfig.mProvider = Provider::ONEDRIVE;
	aConfig.mOneDriveClientId = "onedrive-client";
	return aConfig;
}

// --- devices ---

fs::path gRoot;

fs::path NewDir(std::string_view theName)
{
	static int aCounter = 0;
	fs::path aDir = gRoot / std::format("{}-{}", ++aCounter, theName);
	fs::create_directories(aDir);
	return aDir;
}

std::string ReadText(const fs::path& thePath)
{
	std::ifstream aFile(thePath, std::ios::binary);
	std::ostringstream aStream;
	aStream << aFile.rdbuf();
	return aStream.str();
}

// One computer with the game: its userdata folder, its state folder and the engine syncing them.
struct Device
{
	std::string				mName;
	fs::path				mData;
	fs::path				mState;
	std::unique_ptr<Engine>	mEngine;

	explicit Device(std::string theName) : mName(std::move(theName))
	{
		fs::path aDir = NewDir(mName);
		mData = aDir / "userdata";
		mState = aDir / "state";
		fs::create_directories(mData);
		fs::create_directories(mState);
	}

	void Start(const Config& theConfig, std::unique_ptr<Store> theStore)
	{
		mEngine = std::make_unique<Engine>(theConfig, mData, mState, mName, std::move(theStore));
	}

	void SignIn(Provider theProvider, const std::string& theRefresh, const std::string& theAccount, const std::string& theName = "")
	{
		SaveToken(mState, theProvider, Token{ theRefresh, theAccount, theName });
	}

	void Write(const std::string& theName, const std::string& theData)
	{
		std::ofstream(mData / theName, std::ios::binary | std::ios::trunc) << theData;
	}

	std::string Read(const std::string& theName) const { return ReadText(mData / theName); }
	bool Has(const std::string& theName) const { return fs::exists(mData / theName); }

	Result Sync(bool theAllowPull = true, bool theAllowPush = true)
	{
		return mEngine->Sync(mEngine->ReadLocal(), mName + "'s garden", theAllowPull, theAllowPush);
	}

	Result KeepLocal() { return mEngine->KeepLocal(mEngine->ReadLocal(), mName + "'s garden"); }
	Result UseCloud() { return mEngine->UseCloud(); }
	const std::string& Error() const { return mEngine->GetLastError(); }
};

bool IsManifestPut(const HttpRequest& theRequest)
{
	return theRequest.mMethod == "PUT" && theRequest.mUrl.ends_with("/manifest.txt");
}

// --- tests ---

void TestJson()
{
	Json::Value aJson;
	CHECK(Json::Parse(R"( {"a": "xé😀\n", "n": 42, "s": "17", "f": -1.5e2, "t": true, "z": null,
		"arr": [1, false, {"b": "c"}], "o": {}} )", aJson));
	CHECK_EQ(aJson.Str("a"), "x\xc3\xa9\xf0\x9f\x98\x80\n");
	CHECK_EQ(aJson.Int("n", 0), int64_t(42));
	CHECK_EQ(aJson.Str("n"), "42");
	CHECK_EQ(aJson.Int("s", 0), int64_t(17));	// Google sends 64-bit numbers as strings
	CHECK_EQ(aJson.Int("missing", 7), int64_t(7));
	CHECK_EQ(aJson.Str("missing"), "");
	const Json::Value* anArray = aJson.Find("arr");
	CHECK(anArray && anArray->mArray.size() == 3 && anArray->mArray[2].Str("b") == "c");

	CHECK(!Json::Parse(R"({"a": 1} trailing)", aJson));
	CHECK(!Json::Parse(R"({"a": })", aJson));
	CHECK(!Json::Parse(R"({"a": "unterminated)", aJson));
	CHECK(!Json::Parse("", aJson));
	CHECK(Json::Parse(std::string(32, '[') + std::string(32, ']'), aJson));
	CHECK(!Json::Parse(std::string(100, '[') + std::string(100, ']'), aJson));	// nesting is limited

	std::string aTricky = "a\"b\\c\n\x01/\xc3\xa9";
	CHECK(Json::Parse(Json::Quote(aTricky), aJson) && aJson.mString == aTricky);
	CHECK(Json::Parse(Json::Quote("PvZPortable/files/user1.dat.0123456789abcdef"), aJson) && aJson.mString == "PvZPortable/files/user1.dat.0123456789abcdef");
}

void TestConfig()
{
	fs::path aDir = NewDir("config");
	Config aConfig;
	aConfig.mEnabled = true;
	aConfig.mProvider = Provider::ONEDRIVE;
	aConfig.mFolder = "Saves/PvZ";
	aConfig.mGoogleClientId = "123-abc.apps.googleusercontent.com";
	aConfig.mGoogleClientSecret = "GOCSPX-secret";
	aConfig.mOneDriveClientId = "00000000-1111-2222-3333-444444444444";
	aConfig.mUrl = "https://dav.example/dav";
	aConfig.mPassword = "pass\nword";	// a newline must not start a key of its own
	CHECK(WriteConfig(aDir / "cloudsave.ini", aConfig));

	Config aRead;
	CHECK(LoadConfig(aDir / "cloudsave.ini", aRead));
	CHECK(aRead.mEnabled);
	CHECK(aRead.mProvider == Provider::ONEDRIVE);
	CHECK_EQ(aRead.mFolder, "Saves/PvZ");
	CHECK_EQ(aRead.mGoogleClientId, aConfig.mGoogleClientId);
	CHECK_EQ(aRead.mGoogleClientSecret, aConfig.mGoogleClientSecret);
	CHECK_EQ(aRead.mOneDriveClientId, aConfig.mOneDriveClientId);
	CHECK_EQ(aRead.mUrl, "https://dav.example/dav/");
	CHECK_EQ(aRead.mPassword, "pass");
#ifndef _WIN32
	CHECK((fs::status(aDir / "cloudsave.ini").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

	// a file from before there were other services: no provider key, so WebDAV
	std::ofstream(aDir / "old.ini") << "enabled=1\nurl=https://dav.example/\nusername=alice\npassword=secret\n";
	Config anOld;
	CHECK(LoadConfig(aDir / "old.ini", anOld));
	CHECK(anOld.mProvider == Provider::WEBDAV && anOld.mEnabled && anOld.mFolder == "PvZPortable");

	// WebDAV without a URL can't be on; Google Drive needs none
	std::ofstream(aDir / "nourl.ini") << "enabled=1\nprovider=webdav\n";
	Config aNoUrl;
	CHECK(LoadConfig(aDir / "nourl.ini", aNoUrl) && !aNoUrl.mEnabled);
	std::ofstream(aDir / "google.ini") << "enabled=1\nprovider=google\n";
	Config aGoogle;
	CHECK(LoadConfig(aDir / "google.ini", aGoogle) && aGoogle.mEnabled && aGoogle.mProvider == Provider::GOOGLE_DRIVE);

	// the built-in secret only goes with the built-in client ID
	Config anOwn;
	anOwn.mGoogleClientId = "mine";
	CHECK_EQ(GoogleClientSecret(anOwn), "");
	CHECK(!LoadConfig(aDir / "missing.ini", aRead));
}

void TestTokenFile()
{
	fs::path aDir = NewDir("tokens");
	Token aToken;
	CHECK(!LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK(SaveToken(aDir, Provider::GOOGLE_DRIVE, Token{ "1//refresh", "google-perm", "player@example.com" }));
	CHECK(LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK_EQ(aToken.mRefresh, "1//refresh");
	CHECK_EQ(aToken.mAccount, "google-perm");
	CHECK_EQ(aToken.mAccountName, "player@example.com");
	CHECK(!LoadToken(aDir, Provider::ONEDRIVE, aToken));	// one file per service
#ifndef _WIN32
	CHECK((fs::status(TokenPath(aDir, Provider::GOOGLE_DRIVE)).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
	DeleteToken(aDir, Provider::GOOGLE_DRIVE);
	CHECK(!LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));

	std::ofstream(TokenPath(aDir, Provider::ONEDRIVE)) << "something else\nrefresh x\n";
	CHECK(!LoadToken(aDir, Provider::ONEDRIVE, aToken));
}

void TestDecide()
{
	FileMap aMine{ { "user1.dat", FileEntry{ 3, 1 } } };
	FileMap aTheirs{ { "user1.dat", FileEntry{ 3, 2 } } };
	FileMap aThird{ { "user1.dat", FileEntry{ 3, 3 } } };
	Manifest aBase{ 3, 100, "A", "", aMine };

	auto aRemote = [](uint64_t theRev, const char* theDevice, const FileMap& theFiles)
	{
		return Manifest{ theRev, 200, theDevice, "", theFiles };
	};
	Manifest aSame = aBase;
	CHECK_EQ(Decide(&aBase, aMine, &aSame), Action::NONE);
	Manifest aNewer = aRemote(4, "B", aTheirs);
	CHECK_EQ(Decide(&aBase, aMine, &aNewer), Action::PULL);
	CHECK_EQ(Decide(&aBase, aThird, &aNewer), Action::CONFLICT);
	CHECK_EQ(Decide(&aBase, aThird, &aSame), Action::PUSH);
	// the same revision from another device: both committed at once and this device's commit was overwritten
	Manifest aRace = aRemote(3, "B", aTheirs);
	CHECK_EQ(Decide(&aBase, aMine, &aRace), Action::CONFLICT);
	Manifest anOlder = aRemote(2, "B", aTheirs);
	CHECK_EQ(Decide(&aBase, aMine, &anOlder), Action::CONFLICT);
	Manifest aRaceSameFiles = aRemote(3, "B", aMine);
	CHECK_EQ(Decide(&aBase, aMine, &aRaceSameFiles), Action::NONE);

	CHECK_EQ(Decide(nullptr, {}, &aNewer), Action::PULL);
	CHECK_EQ(Decide(nullptr, aMine, &aNewer), Action::CONFLICT);
	CHECK_EQ(Decide(nullptr, aMine, nullptr), Action::PUSH);
	CHECK_EQ(Decide(nullptr, {}, nullptr), Action::NONE);
}

void TestWebDavSync()
{
	DavServer aServer;
	Device a("dav-a"), b("dav-b");
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	b.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));

	a.Write("users.dat", "profiles");
	a.Write("user1.dat", "level 1-1");
	a.Write("notes.txt", "not a save");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(aServer.Manifest() != nullptr);
	CHECK_EQ(aServer.Blobs(), 2);
	CHECK(aServer.mFolders.contains(DavServer::Root()) && aServer.mFolders.contains(DavServer::Root() + "files/"));
	CHECK_EQ(aServer.mIfMatches.back(), "");	// the first manifest is created with If-None-Match

	// a second computer without saves takes the cloud ones, without being asked
	CHECK_EQ(b.Sync(false), Result::PULL_NEEDED);
	CHECK(!b.Has("user1.dat"));
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "level 1-1");
	CHECK(!b.Has("notes.txt"));

	b.Write("user1.dat", "level 1-2");
	CHECK_EQ(b.Sync(true, false), Result::PUSH_NEEDED);
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK(aServer.mIfMatches.back().starts_with("\"t"));	// replaced only if unchanged
	CHECK_EQ(aServer.Blobs(), 2);	// the replaced blob is gone
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "level 1-2");
	CHECK_EQ(a.Sync(), Result::OK);	// nothing to do

	// both play: the player is asked, and the losing copy is backed up
	a.Write("user1.dat", "level 1-3 on a");
	b.Write("user1.dat", "level 1-3 on b");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	CHECK_EQ(b.mEngine->GetConflict().mRemote.mDevice, "dav-a");
	CHECK_EQ(b.KeepLocal(), Result::OK);
	CHECK_EQ(ReadText(b.mEngine->GetLastBackup() / "user1.dat"), "level 1-3 on a");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "level 1-3 on b");

	a.Write("user1.dat", "level 1-4 on a");
	b.Write("user1.dat", "level 1-4 on b");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	CHECK_EQ(b.UseCloud(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "level 1-4 on a");
	CHECK_EQ(ReadText(b.mEngine->GetLastBackup() / "user1.dat"), "level 1-4 on b");
	CHECK_EQ(b.Sync(), Result::OK);

	// a deleted save is deleted on the other computer too
	fs::remove(a.mData / "users.dat");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK(!b.Has("users.dat"));
}

void TestWebDavRace()
{
	DavServer aServer;
	Device a("race-a"), b("race-b");
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	b.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	a.Write("user1.dat", "start");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);

	// b commits right before a does: a's If-Match no longer matches
	a.Write("user1.dat", "a's change");
	b.Write("user1.dat", "b's change");
	aServer.Before(IsManifestPut, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::CONFLICT);
	CHECK_EQ(a.mEngine->GetConflict().mRemote.mDevice, "race-b");
	CHECK(aServer.Manifest()->mBody.find("device race-b") != std::string::npos);

	// the same, both creating the first manifest
	DavServer aFresh;
	Device c("race-c"), d("race-d");
	c.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aFresh)));
	d.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aFresh)));
	c.Write("user1.dat", "c");
	d.Write("user1.dat", "d");
	aFresh.Before(IsManifestPut, [&] { CHECK_EQ(d.Sync(), Result::OK); });
	CHECK_EQ(c.Sync(), Result::CONFLICT);
	CHECK_EQ(c.mEngine->GetConflict().mRemote.mDevice, "race-d");

	// b commits after a looked but before a uploads: seen by the second look
	a.Write("user1.dat", "a again");
	CHECK_EQ(a.KeepLocal(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);
	a.Write("user1.dat", "a once more");
	b.Write("user1.dat", "b once more");
	aServer.Before([](const HttpRequest& theRequest) { return theRequest.mMethod == "PUT"; }, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::CONFLICT);
}

void TestWebDavWithoutConditions()
{
	// some servers ignore If-Match: the second look before committing is all that stops most races, and a commit that
	// was overwritten anyway is noticed by the device that made it
	DavServer aServer;
	aServer.mConditional = false;
	Device a("plain-a"), b("plain-b");
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	b.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	a.Write("users.dat", "profiles");
	a.Write("user1.dat", "start");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);

	a.Write("user1.dat", "a's level");
	b.Write("game1_1.dat", "b's lawn");
	aServer.Before(IsManifestPut, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::OK);		// overwrote b's commit unknowingly
	CHECK_EQ(b.Sync(), Result::CONFLICT);	// b notices instead of loading a's save over its own
	CHECK_EQ(b.KeepLocal(), Result::OK);
	CHECK_EQ(ReadText(b.mEngine->GetLastBackup() / "user1.dat"), "a's level");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("game1_1.dat"), "b's lawn");
	CHECK_EQ(a.Read("user1.dat"), "start");
}

void TestWebDavWeakETags()
{
	DavServer aServer;
	aServer.mWeakETags = true;
	Device a("weak-a"), b("weak-b");
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	b.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	a.Write("user1.dat", "one");
	CHECK_EQ(a.Sync(), Result::OK);
	a.Write("user1.dat", "two");
	CHECK_EQ(a.Sync(), Result::OK);		// a weak ETag is not sent as If-Match, which would always fail
	CHECK_EQ(aServer.mIfMatches.back(), "");
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "two");
}

void TestWebDavErrors()
{
	DavServer aServer;
	Config aWrong = DavConfig();
	aWrong.mPassword = "wrong";
	Device a("errors-a");
	a.Start(aWrong, MakeWebDavStore(aWrong, LinkTo(aServer)));
	a.Write("user1.dat", "x");
	CHECK_EQ(a.Sync(), Result::FAILED);
	CHECK(a.Error().find("username or password") != std::string::npos);

	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	aServer.mOffline = true;
	CHECK_EQ(a.Sync(), Result::OFFLINE);
	aServer.mOffline = false;
	CHECK_EQ(a.Sync(), Result::OK);

	Config anOff = DavConfig();
	anOff.mEnabled = false;
	a.Start(anOff, MakeWebDavStore(anOff, LinkTo(aServer)));
	CHECK_EQ(a.Sync(), Result::DISABLED);

	// a damaged manifest is reported, not taken for an empty cloud
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	aServer.mFiles[DavServer::Root() + "manifest.txt"].mBody = "PVZP-CLOUDSAVE 1\nfile ../../etc/passwd 3 0123456789abcdef\n";
	CHECK_EQ(a.Sync(), Result::FAILED);
	CHECK(a.Error().find("damaged") != std::string::npos);
}

void TestKeepLocalDamagedCloud()
{
	DavServer aServer;
	Device a("damage-a"), b("damage-b");
	a.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	b.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	a.Write("users.dat", "profiles");
	a.Write("user1.dat", "start");
	a.Write("game1_1.dat", "lawn");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);

	a.Write("user1.dat", "a's level");
	CHECK_EQ(a.Sync(), Result::OK);
	b.Write("user1.dat", "b's level");
	CHECK(aServer.Damage("user1.dat"));
	CHECK(aServer.Damage("users.dat"));		// b has the same users.dat, so it would not upload it again by itself
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	CHECK_EQ(b.UseCloud(), Result::FAILED);
	CHECK(b.Error().find("user1.dat is missing") != std::string::npos);
	CHECK_EQ(b.Read("user1.dat"), "b's level");	// nothing was touched

	CHECK_EQ(b.KeepLocal(), Result::OK);		// what could be read is backed up, and everything is uploaded
	CHECK_EQ(ReadText(b.mEngine->GetLastBackup() / "game1_1.dat"), "lawn");
	CHECK(!fs::exists(b.mEngine->GetLastBackup() / "user1.dat"));
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "b's level");

	Device c("damage-c");	// with nothing of its own, so it needs every file from the cloud
	c.Start(DavConfig(), MakeWebDavStore(DavConfig(), LinkTo(aServer)));
	CHECK_EQ(c.Sync(), Result::OK);
	CHECK_EQ(c.Read("users.dat"), "profiles");
	CHECK_EQ(c.Read("user1.dat"), "b's level");
}

void TestGoogleSignIn()
{
	GoogleServer aServer;
	Link aLink(aServer);
	Config aConfig = GoogleConfig();
	DeviceCode aShown;
	std::vector<int> aWaits;
	Token aToken;
	std::string anError;

	aServer.mPolls = { "pending", "slow_down", "ok" };
	Result aResult = DeviceSignIn(aLink, aConfig, [&](const DeviceCode& theCode) { aShown = theCode; },
		[&](int theSeconds) { aWaits.push_back(theSeconds); return true; }, aToken, anError);
	CHECK_EQ(aResult, Result::OK);
	CHECK_EQ(aShown.mUserCode, "GQVQ-JKEC");
	CHECK_EQ(aShown.mUrl, "https://www.google.com/device");
	CHECK(aWaits == std::vector<int>({ 5, 5, 10 }));	// slow_down adds five seconds
	CHECK(aToken.mRefresh.starts_with("g-refresh-"));
	CHECK_EQ(aToken.mAccount, "google-perm-123");
	CHECK_EQ(aToken.mAccountName, "player@example.com");

	aServer.mPolls = { "pending", "denied" };
	CHECK_EQ(DeviceSignIn(aLink, aConfig, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("declined") != std::string::npos);

	aServer.mPolls = { "expired" };
	CHECK_EQ(DeviceSignIn(aLink, aConfig, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("expired") != std::string::npos);

	// the player waits too long: polling stops when the code runs out
	aServer.mPolls.clear();
	int aPolls = 0;
	CHECK_EQ(DeviceSignIn(aLink, aConfig, [](const DeviceCode&) {}, [&](int) { aPolls++; return true; }, aToken, anError), Result::FAILED);
	CHECK_EQ(aPolls, 1800 / 5);

	CHECK_EQ(DeviceSignIn(aLink, aConfig, [](const DeviceCode&) {}, [](int) { return false; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("cancelled") != std::string::npos);

	Config aWrongSecret = aConfig;
	aWrongSecret.mGoogleClientSecret = "nope";
	aServer.mPolls = { "ok" };
	CHECK_EQ(DeviceSignIn(aLink, aWrongSecret, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("Unauthorized") != std::string::npos);

	Config aNoClient = aConfig;
	aNoClient.mGoogleClientId.clear();
	if (!HasBuiltInClientId(Provider::GOOGLE_DRIVE))
	{
		CHECK_EQ(DeviceSignIn(aLink, aNoClient, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
		CHECK(anError.find("client ID") != std::string::npos);
	}

	aServer.mOffline = true;
	CHECK_EQ(DeviceSignIn(aLink, aConfig, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::OFFLINE);
}

void TestGoogleSync()
{
	GoogleServer aServer;
	aServer.mPageSize = 2;	// several pages of listing
	Device a("google-a"), b("google-b");
	a.SignIn(Provider::GOOGLE_DRIVE, aServer.IssueRefresh(), "google-perm-123");
	b.SignIn(Provider::GOOGLE_DRIVE, aServer.IssueRefresh(), "google-perm-123");
	a.Start(GoogleConfig(), MakeGoogleDriveStore(GoogleConfig(), a.mState, LinkTo(aServer)));
	b.Start(GoogleConfig(), MakeGoogleDriveStore(GoogleConfig(), b.mState, LinkTo(aServer)));

	a.Write("users.dat", "profiles");
	a.Write("user1.dat", "level 1-1");
	a.Write("game1_1.dat", "lawn");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(aServer.mFiles.size(), size_t(4));
	CHECK_EQ(aServer.Named("PvZPortable/manifest.txt").size(), size_t(1));

	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "level 1-1");
	CHECK_EQ(b.Read("game1_1.dat"), "lawn");

	b.Write("user1.dat", "level 1-2");
	fs::remove(b.mData / "game1_1.dat");
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(aServer.mFiles.size(), size_t(3));	// the new blob in, the replaced and the deleted blobs out
	CHECK_EQ(aServer.Named("PvZPortable/manifest.txt").size(), size_t(1));
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "level 1-2");
	CHECK(!a.Has("game1_1.dat"));

	a.Write("user1.dat", "a");
	b.Write("user1.dat", "b");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	CHECK_EQ(b.KeepLocal(), Result::OK);
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "b");

	// a race neither side could stop: b's commit is replaced, and b finds out on its next sync
	a.Write("user1.dat", "a's level");
	b.Write("users.dat", "b's profiles");
	aServer.Before([](const HttpRequest& theRequest) { return theRequest.mMethod == "PATCH"; }, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	// b removed the old users.dat that a's commit still points at: keeping b's copy must work all the same
	CHECK_EQ(b.KeepLocal(), Result::OK);
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("users.dat"), "b's profiles");
	CHECK_EQ(a.Read("user1.dat"), "b");
}

void TestGoogleConditions()
{
	GoogleServer aServer;
	fs::path aStateA = NewDir("gcond-a"), aStateB = NewDir("gcond-b");
	SaveToken(aStateA, Provider::GOOGLE_DRIVE, Token{ aServer.IssueRefresh(), "google-perm-123", "" });
	SaveToken(aStateB, Provider::GOOGLE_DRIVE, Token{ aServer.IssueRefresh(), "google-perm-123", "" });
	std::unique_ptr<Store> a = MakeGoogleDriveStore(GoogleConfig(), aStateA, LinkTo(aServer));
	std::unique_ptr<Store> b = MakeGoogleDriveStore(GoogleConfig(), aStateB, LinkTo(aServer));
	bool aFound = false;
	std::string aBody, aVersionA, aVersionB;

	// both create the first manifest: b's file comes first, so a deletes its own and loses
	aServer.Before([](const HttpRequest& theRequest) { return theRequest.mUrl.find("uploadType=multipart") != std::string::npos; },
		[&] { CHECK_EQ(b->Put(MANIFEST_PATH, "from b", Store::Condition::ABSENT), Result::OK); });
	CHECK_EQ(a->Put(MANIFEST_PATH, "from a", Store::Condition::ABSENT), Result::CONFLICT);
	CHECK_EQ(aServer.Named("PvZPortable/manifest.txt").size(), size_t(1));
	CHECK_EQ(aServer.Manifest()->mBody, "from b");
	CHECK_EQ(a->Get(MANIFEST_PATH, aFound, aBody, aVersionA), Result::OK);
	CHECK(aFound && aBody == "from b");

	// the other way round: a's file is older, b sees it before creating its own
	aServer.mFiles.clear();
	int aLists = 0;
	aServer.Before([&](const HttpRequest& theRequest) { return theRequest.mMethod == "GET" && theRequest.mUrl.find("spaces=appDataFolder") != std::string::npos && ++aLists == 2; },
		[&] { CHECK_EQ(b->Put(MANIFEST_PATH, "from b", Store::Condition::ABSENT), Result::CONFLICT); });
	CHECK_EQ(a->Put(MANIFEST_PATH, "from a", Store::Condition::ABSENT), Result::OK);
	CHECK_EQ(aServer.Named("PvZPortable/manifest.txt").size(), size_t(1));
	CHECK_EQ(aServer.Manifest()->mBody, "from a");

	// replacing only the version that was read
	CHECK_EQ(a->Get(MANIFEST_PATH, aFound, aBody, aVersionA), Result::OK);
	CHECK_EQ(b->Get(MANIFEST_PATH, aFound, aBody, aVersionB), Result::OK);
	CHECK_EQ(aVersionA, aVersionB);
	CHECK_EQ(b->Put(MANIFEST_PATH, "b's", Store::Condition::VERSION, aVersionB), Result::OK);
	CHECK_EQ(a->Put(MANIFEST_PATH, "a's", Store::Condition::VERSION, aVersionA), Result::CONFLICT);
	CHECK_EQ(aServer.Manifest()->mBody, "b's");
	CHECK_EQ(a->Put(MANIFEST_PATH, "a's", Store::Condition::ABSENT), Result::CONFLICT);

	// several files of one name (made by another client): the oldest counts, Delete removes them all
	CHECK_EQ(a->Put("files/user1.dat.0000000000000001", "x"), Result::OK);
	aServer.mFiles.push_back(GoogleServer::File{ "id-dup", "PvZPortable/files/user1.dat.0000000000000001", "2026-11-14T11:00:00.000Z", "y", 1 });
	CHECK_EQ(b->Get("files/user1.dat.0000000000000001", aFound, aBody, aVersionB), Result::OK);
	CHECK_EQ(b->Get(MANIFEST_PATH, aFound, aBody, aVersionB), Result::OK);	// lists again
	CHECK_EQ(b->Get("files/user1.dat.0000000000000001", aFound, aBody, aVersionB), Result::OK);
	CHECK(aFound && aBody == "x");
	CHECK_EQ(b->Delete("files/user1.dat.0000000000000001"), Result::OK);
	CHECK(aServer.Named("PvZPortable/files/user1.dat.0000000000000001").empty());

	CHECK_EQ(a->Get("files/missing.dat.0000000000000001", aFound, aBody, aVersionA), Result::OK);
	CHECK(!aFound);
}

void TestGoogleTokens()
{
	GoogleServer aServer;
	fs::path aState = NewDir("gtokens");
	SaveToken(aState, Provider::GOOGLE_DRIVE, Token{ aServer.IssueRefresh(), "google-perm-123", "" });
	std::unique_ptr<Store> aStore = MakeGoogleDriveStore(GoogleConfig(), aState, LinkTo(aServer));
	bool aFound = false;
	std::string aBody, aVersion;
	CHECK_EQ(aStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::OK);
	CHECK_EQ(aServer.mRefreshes, 1);
	CHECK_EQ(aStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::OK);
	CHECK_EQ(aServer.mRefreshes, 1);	// the access token is kept

	aServer.mAccessTokens.clear();		// expired early: one refresh and the request again
	CHECK_EQ(aStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::OK);
	CHECK_EQ(aServer.mRefreshes, 2);

	aServer.mRefreshTokens.clear();		// revoked
	aServer.mAccessTokens.clear();
	CHECK_EQ(aStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::FAILED);
	CHECK(aStore->GetError().find("sign in again") != std::string::npos);

	fs::path aNone = NewDir("gtokens-none");
	std::unique_ptr<Store> aSignedOut = MakeGoogleDriveStore(GoogleConfig(), aNone, LinkTo(aServer));
	CHECK_EQ(aSignedOut->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::FAILED);
	CHECK(aSignedOut->GetError().find("not signed in") != std::string::npos);
}

void TestOneDriveSignIn()
{
	OneDriveServer aServer;
	Link aLink(aServer);
	DeviceCode aShown;
	std::vector<int> aWaits;
	Token aToken;
	std::string anError;
	aServer.mPolls = { "pending", "pending", "ok" };
	CHECK_EQ(DeviceSignIn(aLink, OneDriveConfig(), [&](const DeviceCode& theCode) { aShown = theCode; },
		[&](int theSeconds) { aWaits.push_back(theSeconds); return true; }, aToken, anError), Result::OK);
	CHECK_EQ(aShown.mUrl, "https://microsoft.com/devicelogin");
	CHECK_EQ(aShown.mUserCode, "ABCD2345");
	CHECK(aWaits == std::vector<int>({ 5, 5, 5 }));
	CHECK(aToken.mRefresh.starts_with("m-refresh-"));
	CHECK_EQ(aToken.mAccount, "drive-42/approot-id");

	aServer.mPolls = { "denied" };
	CHECK_EQ(DeviceSignIn(aLink, OneDriveConfig(), [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("declined") != std::string::npos);

	Config aWrong = OneDriveConfig();
	aWrong.mOneDriveClientId = "someone-else";
	CHECK_EQ(DeviceSignIn(aLink, aWrong, [](const DeviceCode&) {}, [](int) { return true; }, aToken, anError), Result::FAILED);
	CHECK(anError.find("Bad request") != std::string::npos);
}

void TestOneDriveSync()
{
	OneDriveServer aServer;
	Device a("onedrive-a"), b("onedrive-b");
	a.SignIn(Provider::ONEDRIVE, aServer.IssueRefresh(), "drive-42/approot-id");
	b.SignIn(Provider::ONEDRIVE, aServer.IssueRefresh(), "drive-42/approot-id");
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	b.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), b.mState, LinkTo(aServer)));

	a.Write("users.dat", "profiles");
	a.Write("user1.dat", "level 1-1");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(aServer.mItems.contains("PvZPortable") && aServer.mItems["PvZPortable"].mFolder);
	CHECK(aServer.mItems.contains("PvZPortable/files") && aServer.mItems["PvZPortable/files"].mFolder);
	CHECK(aServer.Manifest() != nullptr);

	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "level 1-1");
	CHECK_EQ(aServer.mTokensOnPreAuthorized, 0);	// download and upload addresses get no access token

	// Microsoft replaces the refresh token on every use: the new one is saved, and a restarted game goes on with it
	Token aSaved;
	CHECK(LoadToken(a.mState, Provider::ONEDRIVE, aSaved));
	CHECK(aServer.mRefreshTokens.contains(aSaved.mRefresh));
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	b.Write("user1.dat", "level 1-2");
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "level 1-2");
	CHECK_EQ(aServer.Count("/children"), 4);	// a made both folders, b found them there (409)

	a.Write("user1.dat", "a");
	b.Write("user1.dat", "b");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::CONFLICT);
	CHECK_EQ(b.UseCloud(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "a");

	// only the save files are in the cloud folder, under the folder name
	for (const auto& [aPath, anItem] : aServer.mItems)
		CHECK(aPath == "PvZPortable" || aPath.starts_with("PvZPortable/"));
}

void TestOneDriveConflicts()
{
	OneDriveServer aServer;
	Device a("odc-a"), b("odc-b");
	a.SignIn(Provider::ONEDRIVE, aServer.IssueRefresh(), "drive-42/approot-id");
	b.SignIn(Provider::ONEDRIVE, aServer.IssueRefresh(), "drive-42/approot-id");
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	b.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), b.mState, LinkTo(aServer)));
	auto IsSession = [](const HttpRequest& theRequest) { return theRequest.mUrl.ends_with("/manifest.txt:/createUploadSession"); };
	auto IsUpload = [](const HttpRequest& theRequest) { return theRequest.mUrl.starts_with("https://up.example/"); };

	// both make the first commit; b's upload completes first, so conflictBehavior "fail" turns a's down
	a.Write("user1.dat", "a");
	b.Write("user1.dat", "b");
	aServer.Before(IsUpload, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::CONFLICT);
	CHECK_EQ(a.mEngine->GetConflict().mRemote.mDevice, "odc-b");
	CHECK_EQ(a.UseCloud(), Result::OK);

	// b commits right before a: a's If-Match fails
	a.Write("user1.dat", "a2");
	b.Write("user1.dat", "b2");
	aServer.Before(IsSession, [&] { CHECK_EQ(b.Sync(), Result::OK); });
	CHECK_EQ(a.Sync(), Result::CONFLICT);
	CHECK_EQ(a.UseCloud(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "b2");

	// only the eTag changed, not the commit: a commits on top of it
	a.Write("user1.dat", "a3");
	aServer.Before(IsSession, [&] { aServer.Manifest()->mTag = aServer.mNextTag++; });
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(aServer.Manifest()->mBody.find("device odc-a") != std::string::npos);
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "a3");

	// a server that turns "fail" down at once rather than when the upload completes
	OneDriveServer anEarly;
	anEarly.mConflictAtUpload = false;
	Device c("odc-c"), d("odc-d");
	c.SignIn(Provider::ONEDRIVE, anEarly.IssueRefresh(), "drive-42/approot-id");
	d.SignIn(Provider::ONEDRIVE, anEarly.IssueRefresh(), "drive-42/approot-id");
	c.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), c.mState, LinkTo(anEarly)));
	d.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), d.mState, LinkTo(anEarly)));
	c.Write("user1.dat", "c");
	d.Write("user1.dat", "d");
	anEarly.Before(IsSession, [&] { CHECK_EQ(d.Sync(), Result::OK); });
	CHECK_EQ(c.Sync(), Result::CONFLICT);

	// signed in elsewhere since: the sign-in is gone
	anEarly.mRefreshTokens.clear();
	anEarly.mAccessTokens.clear();
	c.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), c.mState, LinkTo(anEarly)));
	CHECK_EQ(c.Sync(), Result::FAILED);
	CHECK(c.Error().find("sign in again") != std::string::npos);
}

void TestStoreIds()
{
	DavServer aDav;
	GoogleServer aGoogle;
	fs::path aState = NewDir("ids");
	CHECK_EQ(MakeWebDavStore(DavConfig(), LinkTo(aDav))->Id(), "https://dav.example/remote.php/dav/files/alice/PvZPortable/");

	SaveToken(aState, Provider::GOOGLE_DRIVE, Token{ "r", "google-perm-123", "" });
	std::string aFirst = MakeGoogleDriveStore(GoogleConfig(), aState, LinkTo(aGoogle))->Id();
	SaveToken(aState, Provider::GOOGLE_DRIVE, Token{ "r", "google-perm-456", "" });
	std::string aSecond = MakeGoogleDriveStore(GoogleConfig(), aState, LinkTo(aGoogle))->Id();
	CHECK(aFirst != aSecond);	// another account starts a new sync history
	CHECK(aFirst.starts_with("google:"));

	SaveToken(aState, Provider::ONEDRIVE, Token{ "r", "google-perm-456", "" });
	CHECK(MakeOneDriveStore(OneDriveConfig(), aState, LinkTo(aGoogle))->Id() != aSecond);

	// a device that synced with one account and then signs in to another one does not take its old base along
	GoogleServer aServer;
	Device a("ids-a");
	a.SignIn(Provider::GOOGLE_DRIVE, aServer.IssueRefresh(), "google-perm-1");
	a.Start(GoogleConfig(), MakeGoogleDriveStore(GoogleConfig(), a.mState, LinkTo(aServer)));
	a.Write("user1.dat", "mine");
	CHECK_EQ(a.Sync(), Result::OK);
	GoogleServer anOther;
	a.SignIn(Provider::GOOGLE_DRIVE, anOther.IssueRefresh(), "google-perm-2");
	a.Start(GoogleConfig(), MakeGoogleDriveStore(GoogleConfig(), a.mState, LinkTo(anOther)));
	Device b("ids-b");
	b.SignIn(Provider::GOOGLE_DRIVE, anOther.IssueRefresh(), "google-perm-2");
	b.Start(GoogleConfig(), MakeGoogleDriveStore(GoogleConfig(), b.mState, LinkTo(anOther)));
	b.Write("user1.dat", "theirs");
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(a.Sync(), Result::CONFLICT);	// not a pull over "mine" as if it had been synced there
}

}

int main()
{
	gRoot = fs::temp_directory_path() / std::format("cloudsync-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
	fs::create_directories(gRoot);

	const std::pair<const char*, void (*)()> TESTS[] = {
		{ "json", TestJson },
		{ "config", TestConfig },
		{ "token file", TestTokenFile },
		{ "decide", TestDecide },
		{ "webdav sync", TestWebDavSync },
		{ "webdav race", TestWebDavRace },
		{ "webdav without conditions", TestWebDavWithoutConditions },
		{ "webdav weak etags", TestWebDavWeakETags },
		{ "webdav errors", TestWebDavErrors },
		{ "keep local, damaged cloud", TestKeepLocalDamagedCloud },
		{ "google sign-in", TestGoogleSignIn },
		{ "google sync", TestGoogleSync },
		{ "google conditions", TestGoogleConditions },
		{ "google tokens", TestGoogleTokens },
		{ "onedrive sign-in", TestOneDriveSignIn },
		{ "onedrive sync", TestOneDriveSync },
		{ "onedrive conflicts", TestOneDriveConflicts },
		{ "store ids", TestStoreIds },
	};
	for (const auto& [aName, aTest] : TESTS)
	{
		gTest = aName;
		int aBefore = gFailures;
		aTest();
		std::cout << std::format("{:<28} {}\n", aName, gFailures == aBefore ? "ok" : "FAILED");
	}

	std::error_code anError;
	fs::remove_all(gRoot, anError);
	std::cout << std::format("{} checks, {} failed\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
