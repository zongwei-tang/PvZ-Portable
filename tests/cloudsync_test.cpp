/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

// Offline tests of the cloud save protocol: the sync engine against in-memory WebDAV, Google Drive and OneDrive
// servers that answer the way the real ones are documented to. Nothing here leaves this computer: the only sockets
// are those of the sign-in listener's test, on the loopback address.
//
//   cmake -B build -DCLOUD_SAVE_TESTS=ON && cmake --build build --target cloudsync-test && build/cloudsync-test

#include "CloudSync.h"
#include "CloudJson.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace CloudSync;
namespace fs = std::filesystem;

namespace
{

// --- checks ---

int gChecks = 0;
int gFailures = 0;
std::string gTest;
std::string gSkipped;	// why the current test could not run here

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
// another device act in the middle of a sync. Instead gives the first request it matches another answer, once.
struct Server
{
	bool										mOffline = false;
	std::vector<std::string>					mLog;
	std::function<bool(const HttpRequest&)>		mHook;
	std::function<bool(const HttpRequest&)>		mInsteadMatch;
	HttpResponse								mInstead;

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
		if (mInsteadMatch && mInsteadMatch(theRequest))
		{
			mInsteadMatch = nullptr;
			return std::move(mInstead);
		}
		return Handle(theRequest);
	}

	// the server does nothing for that request: theResponse is all there is to it
	void Instead(std::function<bool(const HttpRequest&)> theMatch, HttpResponse theResponse)
	{
		mInsteadMatch = std::move(theMatch);
		mInstead = std::move(theResponse);
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

// --- OAuth ---

// An authorization code the consent screen handed out, and what it was asked for with.
struct Grant
{
	std::string	mChallenge;
	std::string	mRedirect;
	bool		mOffline = true;
};

// theAddress is thePrefix followed by a port, such as http://127.0.0.1:49152.
bool IsLoopbackAddress(const std::string& theAddress, std::string_view thePrefix)
{
	return theAddress.starts_with(thePrefix) && theAddress.size() > thePrefix.size() &&
		std::all_of(theAddress.begin() + static_cast<ptrdiff_t>(thePrefix.size()), theAddress.end(),
			[](char theChar) { return std::isdigit(static_cast<unsigned char>(theChar)) != 0; });
}

// The request line target a browser sends for theUrl: its path and query, "/" for an address with no path, "" for
// no address at all (a provider that showed an error page instead).
std::string Target(const std::string& theUrl)
{
	if (theUrl.empty())
		return "";
	size_t aScheme = theUrl.find("://");
	size_t aStart = theUrl.find_first_of("/?", aScheme == std::string::npos ? 0 : aScheme + 3);
	if (aStart == std::string::npos)
		return "/";
	std::string aTarget = theUrl.substr(aStart);
	return aTarget[0] == '?' ? "/" + aTarget : aTarget;
}

// Plays the player's browser for a sign-in: what the game's listener gets once the player answered.
struct Browser
{
	int							mPort = 49152;
	std::string					mOpenError;		// the listener cannot be opened
	std::string					mWaitError;		// the listener fails while it waits
	std::vector<std::string>	mRequests;		// request targets, such as "/?state=...&code=..."
	int							mOpens = 0;
	std::vector<std::string>	mShown;			// what the browser that came back was shown in the end: done, denied, failed
};

// What theBrowser was shown, in order: "done", "denied,failed" and the like.
std::string Shown(const Browser& theBrowser)
{
	std::string aText;
	for (const std::string& aPage : theBrowser.mShown)
		aText += (aText.empty() ? "" : ",") + aPage;
	return aText;
}

std::string OutcomeName(RedirectListener::Outcome theOutcome)
{
	switch (theOutcome)
	{
	case RedirectListener::Outcome::DONE:	return "done";
	case RedirectListener::Outcome::DENIED:	return "denied";
	case RedirectListener::Outcome::FAILED:	return "failed";
	}
	return "?";
}

class FakeListener final : public RedirectListener
{
	Browser&	mBrowser;
	bool		mAnswering = false;		// the browser that came back waits for Reply

public:
	explicit FakeListener(Browser& theBrowser) : mBrowser(theBrowser) {}
	~FakeListener() override { Reply(Outcome::FAILED); }	// as the real one does

	bool Open(std::string& theError) override
	{
		Reply(Outcome::FAILED);
		mBrowser.mOpens++;
		theError = mBrowser.mOpenError;
		return theError.empty();
	}

	void Reply(Outcome theOutcome) override
	{
		if (!mAnswering)
			return;
		mAnswering = false;
		mBrowser.mShown.push_back(OutcomeName(theOutcome));
	}

	int Port() const override
	{
		return mBrowser.mPort;
	}

	// Like the real one, only an answer to "/" with the state of this sign-in counts.
	bool Wait(const std::string& theState, Query& theQuery, const std::function<bool()>& theKeepWaiting, std::string& theError) override
	{
		theQuery.clear();
		theError.clear();
		for (const std::string& aRequest : mBrowser.mRequests)
		{
			if (!theKeepWaiting())
				return false;
			Url aUrl = ParseUrl(aRequest);
			if (aUrl.mPath == "/" && (aUrl.mQuery.contains("code") || aUrl.mQuery.contains("error")) && aUrl.mQuery["state"] == theState)
			{
				theQuery = aUrl.mQuery;
				mAnswering = true;
				return true;
			}
		}
		if (!mBrowser.mWaitError.empty())
		{
			theError = mBrowser.mWaitError;
			return false;
		}
		auto aGiveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (theKeepWaiting())
		{
			if (std::chrono::steady_clock::now() > aGiveUp)
			{
				Report(false, __LINE__, "the sign-in kept waiting for a browser that sent nothing more");
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
};

// What a sign-in says when the browser never came back, as when the provider showed an error page instead.
const std::string NO_BROWSER = "the browser did not come back in time; if the sign-in page showed an error, check the client settings (see the README)";

// A sign-in with theConfig whose browser comes back with what theAnswer makes of the sign-in page ("": nothing, as
// when the provider shows an error on its own page). theShown gets what that browser was shown in the end, "" when
// it did not come back.
Result SignInWith(Transport& theHttp, const Config& theConfig, const std::function<std::string(const std::string& thePage)>& theAnswer,
	Token& theToken, std::string& theError, std::string* theShown = nullptr, std::chrono::seconds theTimeLimit = std::chrono::minutes(10))
{
	Browser aBrowser;
	Result aResult = Result::FAILED;
	{
		BrowserSignIn aSignIn(theConfig, std::make_unique<FakeListener>(aBrowser));
		std::string aPage;
		aResult = aSignIn.Start(aPage, theError);
		if (aResult == Result::OK)
		{
			std::string aRequest = theAnswer(aPage);
			if (!aRequest.empty())
				aBrowser.mRequests.push_back(aRequest);
			aResult = aSignIn.Finish(theHttp, [] { return true; }, nullptr, nullptr, theToken, theError, theTimeLimit);
		}
	}
	if (theShown)
		*theShown = Shown(aBrowser);
	return aResult;
}

// The answer of a browser whose player said no to the game, or that the provider sent back theError.
std::string Refusal(const std::string& thePage, const std::string& theError, const std::string& theDescription = "")
{
	std::string aRequest = std::format("/?error={}&state={}", UrlEscape(theError), UrlEscape(ParseUrl(thePage).mQuery["state"]));
	if (!theDescription.empty())
		aRequest += "&error_description=" + UrlEscape(theDescription);
	return aRequest;
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
	std::map<std::string, Grant>	mCodes;		// authorization codes not redeemed yet
	std::set<std::string>		mForeignRefresh;		// refresh tokens issued to another client
	bool						mNoRefresh = false;		// answers a code without a refresh token
	int							mRefreshes = 0;
	size_t						mPageSize = 1000;
	std::string					mClientType = "Desktop app";	// as created in the Google Cloud console
	std::string					mGranted = "https://www.googleapis.com/auth/drive.appdata";	// what the player ticked

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

	// The consent screen in the browser: the player picks an account and allows the game. Returns the address the
	// browser is sent back to, or "" for a request Google shows an error page for instead.
	std::string Authorize(const std::string& thePage)
	{
		Url aUrl = ParseUrl(thePage);
		std::map<std::string, std::string>& aQuery = aUrl.mQuery;
		// Desktop app clients may use any port of a loopback address. Another type of client, such as the "TVs and
		// Limited Input devices" one of the device flow, gets an error page; so does a client ID Google does not know.
		if (aUrl.mPath != "https://accounts.google.com/o/oauth2/v2/auth" || aQuery["client_id"] != mClientId || mClientType != "Desktop app" ||
			!IsLoopbackAddress(aQuery["redirect_uri"], "http://127.0.0.1:") || aQuery["response_type"] != "code" ||
			aQuery["scope"] != "https://www.googleapis.com/auth/drive.appdata" || aQuery["code_challenge_method"] != "S256" ||
			aQuery["code_challenge"].size() != 43 || aQuery["state"].empty())
			return "";
		std::string aCode = std::format("4/g-code-{}", ++mSerial);
		mCodes[aCode] = Grant{ aQuery["code_challenge"], aQuery["redirect_uri"], aQuery["access_type"] == "offline" };
		return std::format("{}?state={}&code={}&scope={}", aQuery["redirect_uri"], UrlEscape(aQuery["state"]), UrlEscape(aCode),
			UrlEscape(aQuery["scope"]));
	}

	HttpResponse Token(const HttpRequest& theRequest)
	{
		auto aForm = ParsePairs(Body(theRequest));
		if (aForm["client_id"] != mClientId || aForm["client_secret"] != mSecret)
			return Answer(401, R"({"error":"invalid_client","error_description":"Unauthorized"})");

		if (aForm["grant_type"] == "authorization_code")
		{
			auto aGrant = mCodes.find(aForm["code"]);
			if (aGrant == mCodes.end())
				return Answer(400, R"({"error":"invalid_grant","error_description":"Malformed auth code."})");
			Grant aDone = aGrant->second;
			mCodes.erase(aGrant);	// good for one try
			if (aForm["redirect_uri"] != aDone.mRedirect)
				return Answer(400, R"({"error":"redirect_uri_mismatch","error_description":"Bad Request"})");
			if (Base64Url(Sha256(aForm["code_verifier"])) != aDone.mChallenge)
				return Answer(400, R"({"error":"invalid_grant","error_description":"Invalid code verifier."})");
			std::string anAccess = std::format("g-access-{}", ++mSerial);
			mAccessTokens.insert(anAccess);
			std::string aRefresh = aDone.mOffline && !mNoRefresh ? std::format(R"("refresh_token":"{}",)", IssueRefresh()) : "";
			return Answer(200, std::format(R"({{"access_token":"{}","expires_in":3599,{}"scope":{},"token_type":"Bearer"}})",
				anAccess, aRefresh, Json::Quote(mGranted)));
		}
		if (aForm["grant_type"] == "refresh_token")
		{
			if (mForeignRefresh.contains(aForm["refresh_token"]))
				return Answer(400, R"({"error":"unauthorized_client","error_description":"Unauthorized"})");
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
	std::map<std::string, Grant>		mCodes;			// authorization codes not redeemed yet
	int									mRefreshes = 0;
	bool								mConflictAtUpload = true;	// conflictBehavior "fail" is checked when the upload completes
	int									mNextTag = 1;
	int									mTokensOnPreAuthorized = 0;	// requests to upload or download addresses that carried a token
	// the app registration: the platform its redirect URI is under, and whether http://localhost is that URI
	std::string							mPlatform = "Mobile and desktop applications";
	bool								mLocalhost = true;
	std::string							mRedeemedScope = "none";	// the scope of the last code redeemed, "none" for no scope

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

	// The sign-in page in the browser: the player picks an account and allows the game. Returns the address the
	// browser is sent back to, or "" for a request Microsoft shows an error page for instead.
	std::string Authorize(const std::string& thePage)
	{
		Url aUrl = ParseUrl(thePage);
		std::map<std::string, std::string>& aQuery = aUrl.mQuery;
		// Any port of the http://localhost registered matches. A redirect URI that is not registered (AADSTS50011) and
		// an unknown client ID (AADSTS700016) get an error page.
		if (aUrl.mPath != "https://login.microsoftonline.com/common/oauth2/v2.0/authorize" || aQuery["client_id"] != mClientId || !mLocalhost ||
			!IsLoopbackAddress(aQuery["redirect_uri"], "http://localhost:") || aQuery["response_type"] != "code" ||
			aQuery["response_mode"] != "query" || aQuery["scope"] != "Files.ReadWrite.AppFolder offline_access" ||
			aQuery["code_challenge_method"] != "S256" || aQuery["code_challenge"].size() != 43 || aQuery["state"].empty())
			return "";
		std::string aCode = std::format("M.C5_BAY.2.U.m-code-{}", ++mSerial);
		mCodes[aCode] = Grant{ aQuery["code_challenge"], aQuery["redirect_uri"] };
		return std::format("{}/?code={}&state={}", aQuery["redirect_uri"], UrlEscape(aCode), UrlEscape(aQuery["state"]));
	}

	HttpResponse Token(const HttpRequest& theRequest)
	{
		auto aForm = ParsePairs(Body(theRequest));
		if (aForm["client_id"] != mClientId)
			return Answer(400, R"({"error":"unauthorized_client","error_description":"AADSTS700016: Application not found."})");
		if (aForm.contains("client_secret"))
			return Answer(400, R"({"error":"invalid_request","error_description":"AADSTS90023: Public clients can't send a client secret."})");
		// a redirect URI under "Web" makes the app a confidential client, which has to prove itself
		if (mPlatform == "Web")
			return Answer(401, "{\"error\":\"invalid_client\",\"error_description\":\"AADSTS7000218: The request body must contain the following parameter: 'client_assertion' or 'client_secret'.\\r\\nTrace ID: 3\\r\\nCorrelation ID: 4\"}");

		if (aForm["grant_type"] == "authorization_code")
		{
			mRedeemedScope = aForm.contains("scope") ? aForm["scope"] : "none";
			auto aGrant = mCodes.find(aForm["code"]);
			if (aGrant == mCodes.end())
				return Answer(400, "{\"error\":\"invalid_grant\",\"error_description\":\"AADSTS70000: The provided value for the 'code' parameter is not valid.\\r\\nTrace ID: 1\\r\\nCorrelation ID: 2\"}");
			Grant aDone = aGrant->second;
			mCodes.erase(aGrant);	// good for one try
			if (aForm["redirect_uri"] != aDone.mRedirect)
				return Answer(400, R"({"error":"invalid_grant","error_description":"The redirect URI does not match the one the code was issued for."})");
			if (Base64Url(Sha256(aForm["code_verifier"])) != aDone.mChallenge)
				return Answer(400, R"({"error":"invalid_grant","error_description":"AADSTS501481: The Code_Verifier does not match the code_challenge supplied in the authorization request."})");
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

	// The whole file in one range commits the session. As at Microsoft, a session whose commit failed stays until it
	// expires, and a DELETE on its address cancels it.
	HttpResponse Complete(const HttpRequest& theRequest)
	{
		auto aSession = mSessions.find(theRequest.mUrl);
		if (aSession == mSessions.end())
			return Answer(404, R"({"error":{"code":"itemNotFound","message":"The upload session does not exist"}})");
		if (theRequest.mMethod == "DELETE")
		{
			mSessions.erase(aSession);
			return Answer(204);
		}
		std::string aBody = Body(theRequest);
		if (theRequest.mMethod != "PUT" || Header(theRequest, "Content-Range") != std::format("bytes 0-{}/{}", aBody.size() - 1, aBody.size()))
			return Answer(400, R"({"error":{"code":"invalidRange","message":"Bad Content-Range"}})");

		Session aDone = aSession->second;
		auto anItem = mItems.find(aDone.mPath);
		if (aDone.mFail && anItem != mItems.end())
			return Answer(409, R"({"error":{"code":"nameAlreadyExists","message":"Another file exists with the same name as the uploaded session."}})");
		if (!aDone.mIfMatch.empty() && (anItem == mItems.end() || ETag(anItem->second) != aDone.mIfMatch))
			return Answer(412, R"({"error":{"code":"preconditionFailed","message":"ETag does not match current item's value"}})");
		mSessions.erase(aSession);
		mItems[aDone.mPath] = Item{ false, aBody, mNextTag++ };
		return Answer(201, std::format(R"({{"id":"item","eTag":{}}})", Json::Quote(ETag(mItems[aDone.mPath]))));
	}

	HttpResponse Handle(const HttpRequest& theRequest) override
	{
		Url aUrl = ParseUrl(theRequest.mUrl);
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
	CHECK(SaveToken(aDir, Provider::GOOGLE_DRIVE, Token{ "1//refresh", "google-perm", "player@example.com", "google-client" }));
	CHECK(LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK_EQ(aToken.mRefresh, "1//refresh");
	CHECK_EQ(aToken.mAccount, "google-perm");
	CHECK_EQ(aToken.mAccountName, "player@example.com");
	CHECK_EQ(aToken.mClient, "google-client");
	CHECK(!LoadToken(aDir, Provider::ONEDRIVE, aToken));	// one file per service
#ifndef _WIN32
	CHECK((fs::status(TokenPath(aDir, Provider::GOOGLE_DRIVE)).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

	// a sign-in counts only with the client ID it was made with
	Config aConfig;
	aConfig.mProvider = Provider::GOOGLE_DRIVE;
	aConfig.mGoogleClientId = "google-client";
	CHECK(LoadTokenFor(aDir, aConfig, aToken));
	aConfig.mGoogleClientId = "another-client";
	CHECK(!LoadTokenFor(aDir, aConfig, aToken));
	CHECK(aToken.mRefresh.empty());
	aConfig.mProvider = Provider::ONEDRIVE;
	CHECK(!LoadTokenFor(aDir, aConfig, aToken));
	aConfig.mProvider = Provider::GOOGLE_DRIVE;

	// one from before the client was kept counts with any
	std::ofstream(TokenPath(aDir, Provider::GOOGLE_DRIVE), std::ios::trunc) << "PVZP-CLOUDSAVE-TOKEN 1\naccount google-perm\nname \nrefresh 1//old\n";
	CHECK(LoadTokenFor(aDir, aConfig, aToken));
	CHECK_EQ(aToken.mRefresh, "1//old");
	CHECK_EQ(aToken.mClient, "");

	// a refresh saves its new token only over the one it was made with: not over a sign-in or sign-out made since
	CHECK(ReplaceToken(aDir, Provider::GOOGLE_DRIVE, "1//old", Token{ "1//newer", "google-perm", "", "another-client" }));
	CHECK(LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK_EQ(aToken.mRefresh, "1//newer");
	CHECK_EQ(aToken.mClient, "another-client");
	CHECK(!ReplaceToken(aDir, Provider::GOOGLE_DRIVE, "1//old", Token{ "1//stale", "google-perm", "" }));
	CHECK(LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK_EQ(aToken.mRefresh, "1//newer");
	DeleteToken(aDir, Provider::GOOGLE_DRIVE);
	CHECK(!LoadToken(aDir, Provider::GOOGLE_DRIVE, aToken));
	CHECK(!ReplaceToken(aDir, Provider::GOOGLE_DRIVE, "1//newer", Token{ "1//after", "google-perm", "" }));
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

std::string Hex(std::string_view theBytes)
{
	std::string aText;
	for (char aByte : theBytes)
		aText += std::format("{:02x}", static_cast<unsigned char>(aByte));
	return aText;
}

void TestPkce()
{
	// FIPS 180-2 examples, and lengths on both sides of the 56 and 64 bytes where the padding takes another block
	CHECK_EQ(Hex(Sha256("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK_EQ(Hex(Sha256("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK_EQ(Hex(Sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK_EQ(Hex(Sha256("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
		"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
	CHECK_EQ(Hex(Sha256(std::string(1000000, 'a'))), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	const std::pair<size_t, const char*> LENGTHS[] = {
		{ 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
		{ 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
		{ 63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
		{ 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
		{ 65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
		{ 119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
		{ 120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
		{ 128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
	};
	for (const auto& [aLength, aDigest] : LENGTHS)
		CHECK_EQ(Hex(Sha256(std::string(aLength, 'a'))), aDigest);
	CHECK_EQ(Sha256("abc").size(), size_t(32));

	// RFC 4648 section 10 in the URL alphabet, without padding
	CHECK_EQ(Base64Url(""), "");
	CHECK_EQ(Base64Url("f"), "Zg");
	CHECK_EQ(Base64Url("fo"), "Zm8");
	CHECK_EQ(Base64Url("foo"), "Zm9v");
	CHECK_EQ(Base64Url("foob"), "Zm9vYg");
	CHECK_EQ(Base64Url("fooba"), "Zm9vYmE");
	CHECK_EQ(Base64Url("foobar"), "Zm9vYmFy");
	CHECK_EQ(Base64Url("\xfb\xff"), "-_8");
	CHECK_EQ(Base64Url("\xfb\xef\xbe"), "----");
	CHECK_EQ(Base64Url("\xff\xff\xff"), "____");
	CHECK_EQ(Base64Url(std::string("\0\0\0", 3)), "AAAA");

	// RFC 7636 appendix B
	CHECK_EQ(Base64Url(Sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

void TestGoogleSignIn()
{
	GoogleServer aServer;
	Link aLink(aServer);
	Config aConfig = GoogleConfig();
	auto aKeepWaiting = [] { return true; };
	auto anAllow = [&](const std::string& thePage) { return Target(aServer.Authorize(thePage)); };
	Token aToken;
	std::string aPage, anError, aShown;

	// the player allows the game; a stale tab of an earlier sign-in and the favicon reach the listener first
	{
		Browser aBrowser;
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
		CHECK_EQ(aBrowser.mOpens, 1);
		CHECK(aPage.starts_with("https://accounts.google.com/o/oauth2/v2/auth?"));
		Url aUrl = ParseUrl(aPage);
		CHECK_EQ(aUrl.mQuery["client_id"], "google-client");
		CHECK_EQ(aUrl.mQuery["redirect_uri"], "http://127.0.0.1:49152");
		CHECK_EQ(aUrl.mQuery["response_type"], "code");
		CHECK_EQ(aUrl.mQuery["scope"], "https://www.googleapis.com/auth/drive.appdata");
		CHECK_EQ(aUrl.mQuery["code_challenge_method"], "S256");
		CHECK_EQ(aUrl.mQuery["code_challenge"].size(), size_t(43));
		CHECK_EQ(aUrl.mQuery["state"].size(), size_t(22));
		CHECK_EQ(aUrl.mQuery["access_type"], "offline");
		CHECK_EQ(aUrl.mQuery["prompt"], "consent select_account");
		CHECK(aPage.find("google-secret") == std::string::npos);	// the secret goes to the token request only

		std::string aRedirect = aServer.Authorize(aPage);
		CHECK(aRedirect.starts_with("http://127.0.0.1:49152?state="));
		aBrowser.mRequests = { "/?state=stale&code=old", "/favicon.ico", Target(aRedirect) };
		int anAnswers = 0;
		Token aKept;
		CHECK_EQ(aSignIn.Finish(aLink, aKeepWaiting, [&] { anAnswers++; CHECK_EQ(Shown(aBrowser), ""); },
			[&](const Token& theToken) { aKept = theToken; CHECK_EQ(Shown(aBrowser), ""); return true; }, aToken, anError), Result::OK);
		CHECK_EQ(anError, "");
		CHECK_EQ(anAnswers, 1);
		CHECK_EQ(Shown(aBrowser), "done");		// once the sign-in was fetched and kept, not before
		CHECK(aToken.mRefresh.starts_with("g-refresh-"));
		CHECK(aServer.mRefreshTokens.contains(aToken.mRefresh));
		CHECK_EQ(aToken.mAccount, "google-perm-123");
		CHECK_EQ(aToken.mAccountName, "player@example.com");
		CHECK_EQ(aToken.mClient, "google-client");
		CHECK_EQ(aKept.mRefresh, aToken.mRefresh);
		CHECK_EQ(aKept.mClient, "google-client");
		CHECK(aServer.mCodes.empty());		// redeemed with the verifier and the address it was issued for

		CHECK_EQ(aSignIn.Finish(aLink, aKeepWaiting, nullptr, nullptr, aToken, anError), Result::FAILED);	// a sign-in finishes once
		CHECK(anError.find("not started") != std::string::npos);
	}

	// every sign-in has a state and a verifier of its own: the answer meant for one does not finish another
	{
		Browser aFirstBrowser, aSecondBrowser;
		BrowserSignIn aFirst(aConfig, std::make_unique<FakeListener>(aFirstBrowser));
		BrowserSignIn aSecond(aConfig, std::make_unique<FakeListener>(aSecondBrowser));
		std::string aFirstPage, aSecondPage;
		CHECK_EQ(aFirst.Start(aFirstPage, anError), Result::OK);
		CHECK_EQ(aSecond.Start(aSecondPage, anError), Result::OK);
		Url aFirstUrl = ParseUrl(aFirstPage), aSecondUrl = ParseUrl(aSecondPage);
		CHECK(aFirstUrl.mQuery["state"] != aSecondUrl.mQuery["state"]);
		CHECK(aFirstUrl.mQuery["code_challenge"] != aSecondUrl.mQuery["code_challenge"]);
		aSecondBrowser.mRequests = { Target(aServer.Authorize(aFirstPage)) };
		CHECK_EQ(aSecond.Finish(aLink, aKeepWaiting, nullptr, nullptr, aToken, anError, std::chrono::seconds(1)), Result::FAILED);
		CHECK_EQ(anError, NO_BROWSER);
		CHECK(aToken.mRefresh.empty());
		CHECK_EQ(Shown(aSecondBrowser), "");
		aServer.mCodes.clear();
	}

	// the player says no
	size_t aRequests = aServer.mLog.size();
	CHECK_EQ(SignInWith(aLink, aConfig, [](const std::string& thePage) { return Refusal(thePage, "access_denied"); }, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "the game was not given access to your Google Drive");
	CHECK_EQ(aShown, "denied");
	CHECK_EQ(aServer.mLog.size(), aRequests);	// nothing to redeem

	// or signs in without ticking the box for Drive; a scope among others is fine
	aServer.mGranted = "openid https://www.googleapis.com/auth/userinfo.email";
	CHECK_EQ(SignInWith(aLink, aConfig, anAllow, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "the game was not given access to your Google Drive: tick the box for it on the Google page when you sign in again");
	CHECK_EQ(aShown, "denied");
	CHECK(aToken.mRefresh.empty());
	aServer.mGranted = "openid https://www.googleapis.com/auth/drive.appdata";
	CHECK_EQ(SignInWith(aLink, aConfig, anAllow, aToken, anError, &aShown), Result::OK);
	CHECK_EQ(aShown, "done");
	aServer.mGranted = "https://www.googleapis.com/auth/drive.appdata";

	// an error that Google sends back to the game instead of showing it
	CHECK_EQ(SignInWith(aLink, aConfig, [](const std::string& thePage) { return Refusal(thePage, "server_error"); }, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "Google Drive turned down the sign-in: server_error");
	CHECK_EQ(aShown, "failed");

	// back with the state of the sign-in, but with an empty code: nothing to redeem
	aRequests = aServer.mLog.size();
	CHECK_EQ(SignInWith(aLink, aConfig, [](const std::string& thePage)
		{ return std::format("/?state={}&code=", UrlEscape(ParseUrl(thePage).mQuery["state"])); }, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "Google Drive sent the browser back without a sign-in code");
	CHECK_EQ(aShown, "failed");
	CHECK_EQ(aServer.mLog.size(), aRequests);

	// the code was issued for another verifier, or for another address; the code of the error goes first
	CHECK_EQ(SignInWith(aLink, aConfig, [&](const std::string& thePage)
		{
			std::string aRequest = Target(aServer.Authorize(thePage));
			aServer.mCodes.begin()->second.mChallenge = Base64Url(Sha256("someone else's verifier"));
			return aRequest;
		}, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "signing in to Google Drive failed: invalid_grant: Invalid code verifier.");
	CHECK_EQ(aShown, "failed");
	CHECK_EQ(SignInWith(aLink, aConfig, [&](const std::string& thePage)
		{
			std::string aRequest = Target(aServer.Authorize(thePage));
			aServer.mCodes.begin()->second.mRedirect = "http://127.0.0.1:1";
			return aRequest;
		}, aToken, anError), Result::FAILED);
	CHECK_EQ(anError, "signing in to Google Drive failed: redirect_uri_mismatch: Bad Request");

	// no answer: the player closed the tab and waited, or cancelled in the game
	CHECK_EQ(SignInWith(aLink, aConfig, [](const std::string&) { return ""; }, aToken, anError, &aShown, std::chrono::seconds(0)), Result::FAILED);
	CHECK_EQ(anError, NO_BROWSER);
	CHECK_EQ(aShown, "");
	{
		Browser aBrowser;
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
		aBrowser.mRequests = { Target(aServer.Authorize(aPage)) };
		bool anAnswered = false;
		CHECK_EQ(aSignIn.Finish(aLink, [] { return false; }, [&] { anAnswered = true; }, nullptr, aToken, anError), Result::FAILED);
		CHECK_EQ(anError, "the sign-in was cancelled");
		CHECK(!anAnswered);
		CHECK_EQ(Shown(aBrowser), "");
		aServer.mCodes.clear();
	}

	// Google shows the error on its own page, and the browser never comes back, for a client of another type (the
	// "TVs and Limited Input devices" one of the device flow, say) and for a client ID it does not know
	aServer.mClientType = "TVs and Limited Input devices";
	CHECK_EQ(SignInWith(aLink, aConfig, anAllow, aToken, anError, &aShown, std::chrono::seconds(0)), Result::FAILED);
	CHECK_EQ(anError, NO_BROWSER);
	CHECK_EQ(aShown, "");
	aServer.mClientType = "Desktop app";
	Config aStranger = aConfig;
	aStranger.mGoogleClientId = "someone-else";
	CHECK_EQ(SignInWith(aLink, aStranger, anAllow, aToken, anError, &aShown, std::chrono::seconds(0)), Result::FAILED);
	CHECK_EQ(anError, NO_BROWSER);

	// stopped while the sign-in was fetched: nothing is kept, and the browser is told it failed
	{
		bool aStopped = false;
		aServer.Before([](const HttpRequest& theRequest) { return theRequest.mUrl == "https://oauth2.googleapis.com/token"; }, [&] { aStopped = true; });
		Browser aBrowser;
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
		aBrowser.mRequests = { Target(aServer.Authorize(aPage)) };
		int aKept = 0;
		CHECK_EQ(aSignIn.Finish(aLink, [&] { return !aStopped; }, nullptr, [&](const Token&) { aKept++; return true; }, aToken, anError), Result::FAILED);
		CHECK(aStopped);
		CHECK_EQ(anError, "the sign-in was cancelled");
		CHECK_EQ(aKept, 0);
		CHECK(aToken.mRefresh.empty());
		CHECK_EQ(Shown(aBrowser), "failed");
	}

	// the sign-in could not be kept: the browser must not say it is done
	{
		Browser aBrowser;
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
		aBrowser.mRequests = { Target(aServer.Authorize(aPage)) };
		CHECK_EQ(aSignIn.Finish(aLink, aKeepWaiting, nullptr, [](const Token&) { return false; }, aToken, anError), Result::FAILED);
		CHECK_EQ(anError, "the sign-in could not be saved");
		CHECK(aToken.mRefresh.empty());
		CHECK_EQ(Shown(aBrowser), "failed");
	}

	// the listener cannot be opened, or fails while it waits
	{
		Browser aBrowser;
		aBrowser.mOpenError = "could not listen on 127.0.0.1 for the browser: Operation not permitted";
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		aPage = "left over";
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::FAILED);
		CHECK_EQ(anError, aBrowser.mOpenError);
		CHECK_EQ(aPage, "");
		CHECK_EQ(aSignIn.Finish(aLink, aKeepWaiting, nullptr, nullptr, aToken, anError), Result::FAILED);
		CHECK(anError.find("not started") != std::string::npos);
	}
	{
		Browser aBrowser;
		aBrowser.mWaitError = "waiting for the browser failed: Bad file descriptor";
		BrowserSignIn aSignIn(aConfig, std::make_unique<FakeListener>(aBrowser));
		CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
		CHECK_EQ(aSignIn.Finish(aLink, aKeepWaiting, nullptr, nullptr, aToken, anError), Result::FAILED);
		CHECK_EQ(anError, aBrowser.mWaitError);
		CHECK_EQ(Shown(aBrowser), "");
	}

	// the token request
	Config aWrongSecret = aConfig;
	aWrongSecret.mGoogleClientSecret = "nope";
	CHECK_EQ(SignInWith(aLink, aWrongSecret, anAllow, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "Google did not accept the client secret: enter the secret of this client ID (see the README)");
	CHECK_EQ(aShown, "failed");
	aServer.mCodes.clear();

	aServer.mNoRefresh = true;
	CHECK_EQ(SignInWith(aLink, aConfig, anAllow, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "Google Drive did not let the game stay signed in");
	CHECK_EQ(aShown, "failed");
	aServer.mNoRefresh = false;

	CHECK_EQ(SignInWith(aLink, aConfig, [&](const std::string& thePage)
		{
			std::string aRequest = Target(aServer.Authorize(thePage));
			aServer.mOffline = true;
			return aRequest;
		}, aToken, anError, &aShown), Result::OFFLINE);
	CHECK_EQ(aShown, "failed");
	aServer.mOffline = false;
	aServer.mCodes.clear();

	// what has to be set up first
	Config aNoSecret = aConfig;
	aNoSecret.mGoogleClientSecret.clear();
	Browser aBrowser;
	CHECK_EQ(BrowserSignIn(aNoSecret, std::make_unique<FakeListener>(aBrowser)).Start(aPage, anError), Result::FAILED);
	CHECK_EQ(anError, "enter the client secret of your Google app first (see the README)");
	Config aNoClient = aConfig;
	aNoClient.mGoogleClientId.clear();
	if (!HasBuiltInClientId(Provider::GOOGLE_DRIVE))
	{
		CHECK_EQ(BrowserSignIn(aNoClient, std::make_unique<FakeListener>(aBrowser)).Start(aPage, anError), Result::FAILED);
		CHECK(anError.find("client ID") != std::string::npos);
	}
	Config aWebDav = aConfig;
	aWebDav.mProvider = Provider::WEBDAV;
	CHECK_EQ(BrowserSignIn(aWebDav, std::make_unique<FakeListener>(aBrowser)).Start(aPage, anError), Result::FAILED);
	CHECK_EQ(aBrowser.mOpens, 0);	// nothing listens before the sign-in can go ahead
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
	Token aSaved;
	CHECK(LoadToken(aState, Provider::GOOGLE_DRIVE, aSaved));
	CHECK_EQ(aSaved.mClient, "google-client");		// learned from the first refresh, as in a file from before it was kept
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

	// made with another client, such as the "TVs and Limited Input devices" one of earlier versions, in a file that
	// does not say so: Google turns its refresh token down for this client
	fs::path aForeign = NewDir("gtokens-foreign");
	aServer.mForeignRefresh.insert("tv-refresh");
	SaveToken(aForeign, Provider::GOOGLE_DRIVE, Token{ "tv-refresh", "google-perm-123", "" });
	std::unique_ptr<Store> aForeignStore = MakeGoogleDriveStore(GoogleConfig(), aForeign, LinkTo(aServer));
	CHECK_EQ(aForeignStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::FAILED);
	CHECK_EQ(aForeignStore->GetError(), "the Google Drive sign-in was made with another client ID: press Sign Out, then Sign In again");

	// a secret that does not go with the client ID
	Config aWrongSecret = GoogleConfig();
	aWrongSecret.mGoogleClientSecret = "nope";
	fs::path aSecretState = NewDir("gtokens-secret");
	SaveToken(aSecretState, Provider::GOOGLE_DRIVE, Token{ aServer.IssueRefresh(), "google-perm-123", "" });
	std::unique_ptr<Store> aWrongStore = MakeGoogleDriveStore(aWrongSecret, aSecretState, LinkTo(aServer));
	CHECK_EQ(aWrongStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::FAILED);
	CHECK_EQ(aWrongStore->GetError(), "Google did not accept the client ID and secret: check that the secret belongs to this client ID (see the README)");

	// kept with another client ID than the one set now: not signed in, and nothing is sent
	fs::path anOtherClient = NewDir("gtokens-other-client");
	SaveToken(anOtherClient, Provider::GOOGLE_DRIVE, Token{ aServer.IssueRefresh(), "google-perm-123", "", "old-client" });
	size_t aRequests = aServer.mLog.size();
	std::unique_ptr<Store> anOtherStore = MakeGoogleDriveStore(GoogleConfig(), anOtherClient, LinkTo(aServer));
	CHECK_EQ(anOtherStore->Get(MANIFEST_PATH, aFound, aBody, aVersion), Result::FAILED);
	CHECK(anOtherStore->GetError().find("not signed in") != std::string::npos);
	CHECK_EQ(aServer.mLog.size(), aRequests);
}

void TestOneDriveSignIn()
{
	OneDriveServer aServer;
	Link aLink(aServer);
	auto anAllow = [&](const std::string& thePage) { return Target(aServer.Authorize(thePage)); };
	Token aToken;
	std::string aPage, anError, aShown;

	Browser aBrowser;
	BrowserSignIn aSignIn(OneDriveConfig(), std::make_unique<FakeListener>(aBrowser));
	CHECK_EQ(aSignIn.Start(aPage, anError), Result::OK);
	CHECK(aPage.starts_with("https://login.microsoftonline.com/common/oauth2/v2.0/authorize?"));
	Url aUrl = ParseUrl(aPage);
	CHECK_EQ(aUrl.mQuery["client_id"], "onedrive-client");
	CHECK_EQ(aUrl.mQuery["redirect_uri"], "http://localhost:49152");
	CHECK_EQ(aUrl.mQuery["response_type"], "code");
	CHECK_EQ(aUrl.mQuery["response_mode"], "query");
	CHECK_EQ(aUrl.mQuery["scope"], "Files.ReadWrite.AppFolder offline_access");
	CHECK_EQ(aUrl.mQuery["code_challenge_method"], "S256");
	CHECK_EQ(aUrl.mQuery["prompt"], "select_account");
	CHECK(!aUrl.mQuery.contains("client_secret"));
	std::string aRedirect = aServer.Authorize(aPage);
	CHECK(aRedirect.starts_with("http://localhost:49152/?code="));
	aBrowser.mRequests = { Target(aRedirect) };
	CHECK_EQ(aSignIn.Finish(aLink, [] { return true; }, nullptr, nullptr, aToken, anError), Result::OK);	// with no client secret
	CHECK(aToken.mRefresh.starts_with("m-refresh-"));
	CHECK_EQ(aToken.mAccount, "drive-42/approot-id");
	CHECK_EQ(aToken.mClient, "onedrive-client");
	CHECK(aServer.mCodes.empty());
	CHECK_EQ(aServer.mRedeemedScope, "Files.ReadWrite.AppFolder offline_access");	// sent again, as Microsoft's example does
	CHECK_EQ(aServer.Count("GET https://graph.microsoft.com/v1.0/me/drive/special/approot?$select=id,parentReference"), 1);	// "$" as OData has it
	CHECK_EQ(Shown(aBrowser), "done");

	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), [](const std::string& thePage) { return Refusal(thePage, "access_denied", "AADSTS65004: User declined to consent to access the app."); },
		aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "the game was not given access to your OneDrive");
	CHECK_EQ(aShown, "denied");

	// an error that Microsoft sends back to the game, its description running over several lines of trace ids
	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), [](const std::string& thePage)
		{ return Refusal(thePage, "temporarily_unavailable", "AADSTS90033: A transient error has occurred. Please try again.\r\nTrace ID: 1\r\nCorrelation ID: 2"); },
		aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "OneDrive turned down the sign-in: AADSTS90033: A transient error has occurred. Please try again.");
	CHECK_EQ(aShown, "failed");

	// a redirect URI that is not registered (AADSTS50011) and a client ID Microsoft does not know (AADSTS700016)
	// are shown on its own page, and the browser never comes back
	aServer.mLocalhost = false;
	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), anAllow, aToken, anError, &aShown, std::chrono::seconds(0)), Result::FAILED);
	CHECK_EQ(anError, NO_BROWSER);
	CHECK_EQ(aShown, "");
	aServer.mLocalhost = true;
	Config aStranger = OneDriveConfig();
	aStranger.mOneDriveClientId = "someone-else";
	CHECK_EQ(SignInWith(aLink, aStranger, anAllow, aToken, anError, &aShown, std::chrono::seconds(0)), Result::FAILED);
	CHECK_EQ(anError, NO_BROWSER);

	// http://localhost registered under "Web" rather than "Mobile and desktop applications"
	aServer.mPlatform = "Web";
	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), anAllow, aToken, anError, &aShown), Result::FAILED);
	CHECK_EQ(anError, "signing in to OneDrive failed: invalid_client: AADSTS7000218: The request body must contain the following parameter: 'client_assertion' or 'client_secret'.");
	CHECK_EQ(aShown, "failed");
	aServer.mPlatform = "Mobile and desktop applications";
	aServer.mCodes.clear();

	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), [&](const std::string& thePage)
		{
			std::string aRequest = Target(aServer.Authorize(thePage));
			aServer.mCodes.begin()->second.mChallenge = Base64Url(Sha256("someone else's verifier"));
			return aRequest;
		}, aToken, anError, &aShown), Result::FAILED);
	CHECK(anError.find("AADSTS501481") != std::string::npos);
	CHECK_EQ(aShown, "failed");
	CHECK_EQ(SignInWith(aLink, OneDriveConfig(), [&](const std::string& thePage)
		{
			std::string aRequest = Target(aServer.Authorize(thePage));
			aServer.mCodes.clear();		// already redeemed, or expired
			return aRequest;
		}, aToken, anError), Result::FAILED);
	CHECK_EQ(anError, "signing in to OneDrive failed: invalid_grant: AADSTS70000: The provided value for the 'code' parameter is not valid.");

	Config aNoClient = OneDriveConfig();
	aNoClient.mOneDriveClientId.clear();
	if (!HasBuiltInClientId(Provider::ONEDRIVE))
	{
		CHECK_EQ(SignInWith(aLink, aNoClient, [](const std::string&) { return ""; }, aToken, anError), Result::FAILED);
		CHECK_EQ(anError, "enter the client ID of your OneDrive app first (see the README)");
	}
}

// --- the loopback listener ---

#ifdef _WIN32
using Socket = SOCKET;
const Socket NO_SOCKET = INVALID_SOCKET;
using AddressLength = int;
void CloseSocket(Socket theSocket) { closesocket(theSocket); }
#else
using Socket = int;
const Socket NO_SOCKET = -1;
using AddressLength = socklen_t;
void CloseSocket(Socket theSocket) { close(theSocket); }
#endif

// A blocking connection to the loopback address of theFamily, which gives up reading after five seconds.
Socket Connect(int theFamily, int thePort)
{
	Socket aSocket = socket(theFamily, SOCK_STREAM, IPPROTO_TCP);
	if (aSocket == NO_SOCKET)
		return NO_SOCKET;
#ifdef _WIN32
	DWORD aTimeout = 5000;
#else
	timeval aTimeout{ 5, 0 };
#endif
	setsockopt(aSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&aTimeout), sizeof(aTimeout));

	sockaddr_storage anAddress{};
	AddressLength aLength;
	if (theFamily == AF_INET)
	{
		sockaddr_in* anIPv4 = reinterpret_cast<sockaddr_in*>(&anAddress);
		anIPv4->sin_family = AF_INET;
		anIPv4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		anIPv4->sin_port = htons(static_cast<uint16_t>(thePort));
		aLength = sizeof(sockaddr_in);
	}
	else
	{
		sockaddr_in6* anIPv6 = reinterpret_cast<sockaddr_in6*>(&anAddress);
		anIPv6->sin6_family = AF_INET6;
		anIPv6->sin6_addr.s6_addr[15] = 1;
		anIPv6->sin6_port = htons(static_cast<uint16_t>(thePort));
		aLength = sizeof(sockaddr_in6);
	}
	if (connect(aSocket, reinterpret_cast<sockaddr*>(&anAddress), aLength) != 0)
	{
		CloseSocket(aSocket);
		return NO_SOCKET;
	}
	return aSocket;
}

// Sends thePieces, with a pause between them, and reads the answer until the listener closes the connection.
std::string Exchange(int theFamily, int thePort, const std::vector<std::string>& thePieces)
{
	Socket aSocket = Connect(theFamily, thePort);
	if (aSocket == NO_SOCKET)
		return "no connection";
	for (size_t i = 0; i < thePieces.size(); i++)
	{
		if (i > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
		send(aSocket, thePieces[i].data(), static_cast<int>(thePieces[i].size()), 0);
	}
	std::string anAnswer;
	char aBuffer[4096];
	for (;;)
	{
		int aCount = static_cast<int>(recv(aSocket, aBuffer, sizeof(aBuffer), 0));
		if (aCount <= 0)
			break;
		anAnswer.append(aBuffer, static_cast<size_t>(aCount));
	}
	CloseSocket(aSocket);
	return anAnswer;
}

// Whether this computer has an IPv6 loopback address to listen on at all.
bool HasIPv6Loopback()
{
	Socket aSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (aSocket == NO_SOCKET)
		return false;
	sockaddr_in6 anAddress{};
	anAddress.sin6_family = AF_INET6;
	anAddress.sin6_addr.s6_addr[15] = 1;
	bool aBound = bind(aSocket, reinterpret_cast<sockaddr*>(&anAddress), sizeof(anAddress)) == 0;
	CloseSocket(aSocket);
	return aBound;
}

std::string StatusLine(const std::string& theAnswer)
{
	return theAnswer.substr(0, theAnswer.find("\r\n"));
}

// A sandbox that allows no listening skips a test of the listener; any other reason it did not open fails it.
void NotListening(int theLine, const std::string& theError)
{
#ifdef _WIN32
	bool aForbidden = theError.ends_with("Winsock error 10013");	// WSAEACCES
#else
	bool aForbidden = theError.ends_with(std::strerror(EPERM)) || theError.ends_with(std::strerror(EACCES));
#endif
	if (aForbidden)
		gSkipped = theError;
	else
		Report(false, theLine, theError);
}

void TestLoopback()
{
	std::unique_ptr<RedirectListener> aListener = MakeLoopbackListener();
	std::string anError;
	Query aQuery;
	CHECK(!aListener->Wait("s", aQuery, [] { return true; }, anError));	// not open yet
	CHECK_EQ(anError, "the game is not listening for the browser");
	if (!aListener->Open(anError))
	{
		NotListening(__LINE__, anError);
		return;
	}
	int aPort = aListener->Port();
	CHECK(aPort >= 1024);		// browsers turn the ports below down
	bool anIPv6 = HasIPv6Loopback();
	const std::string STATE = "Zt0rV1q8yU2bW5nJ4kHc3A";

	// A browser: a spare connection that sends nothing, requests that are not the answer, and then the answer. That
	// one carries what cookies other pages on localhost left, tens of KB at times, and ends in pieces.
	std::vector<std::string> anAnswers;
	std::atomic<bool> aBrowserDone{ false };
	std::thread aBrowser([&]
	{
		Socket aSpare = Connect(AF_INET, aPort);
		anAnswers.push_back(Exchange(AF_INET, aPort, { "GET /favicon.ico HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { "GET /?state=Zt0rV1q8yU2bW5nJ4kHc3", "B&code=x HTTP/1.1\r\nHost: 127.0.0.1\r\n", "\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { std::format("POST /?state={}&code=x HTTP/1.1\r\nContent-Length: 0\r\n\r\n", STATE) }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { std::format("GET /?state={} HTTP/1.1\r", STATE), "\n\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { "nonsense\r\n\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { "GET /?" + std::string(64 * 1024, 'a') + " HTTP/1.1\r\n\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { "GET / HTTP/1.1\r\nCookie: " + std::string(1024 * 1024, 'c') + "\r\n\r\n" }));
		anAnswers.push_back(Exchange(AF_INET, aPort, { std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) }));	// a TLS hello
		anAnswers.push_back(Exchange(anIPv6 ? AF_INET6 : AF_INET, aPort,
			{ std::format("GET /?code=4%2Fa+b&state={}&scope=x&code=second HTTP/1.1\r\nHost: localhost:{}\r\nCookie: {}", STATE, aPort, std::string(30 * 1024, 'c')),
				"\r\n\r", "\n" }));
		aBrowserDone = true;
		if (aSpare != NO_SOCKET)
			CloseSocket(aSpare);
	});
	auto aDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	bool anAnswered = aListener->Wait(STATE, aQuery, [&] { return std::chrono::steady_clock::now() < aDeadline; }, anError);
	CHECK(anAnswered);
	CHECK_EQ(anError, "");
	CHECK_EQ(aQuery["code"], "4/a b");		// decoded, and the first one counts
	CHECK_EQ(aQuery["scope"], "x");
	CHECK_EQ(aQuery["state"], STATE);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK(!aBrowserDone);		// the browser waits to be told how it went
	aListener->Reply(RedirectListener::Outcome::DONE);
	aListener->Reply(RedirectListener::Outcome::FAILED);	// told once only
	aBrowser.join();

	CHECK_EQ(anAnswers.size(), size_t(9));
	if (anAnswers.size() == 9)
	{
		CHECK_EQ(StatusLine(anAnswers[0]), "HTTP/1.1 404 Not Found");
		CHECK(anAnswers[0].find("<link rel=\"icon\" href=\"data:,\">") != std::string::npos);	// so no browser asks for one
		CHECK_EQ(StatusLine(anAnswers[1]), "HTTP/1.1 400 Bad Request");		// another state, sent in pieces
		CHECK(anAnswers[1].find("does not belong to the sign-in") != std::string::npos);
		CHECK_EQ(StatusLine(anAnswers[2]), "HTTP/1.1 405 Method Not Allowed");
		CHECK_EQ(StatusLine(anAnswers[3]), "HTTP/1.1 400 Bad Request");		// neither a code nor an error, and its line ends in two pieces
		CHECK_EQ(StatusLine(anAnswers[4]), "HTTP/1.1 400 Bad Request");
		CHECK_EQ(StatusLine(anAnswers[5]), "HTTP/1.1 414 URI Too Long");
		CHECK_EQ(StatusLine(anAnswers[6]), "HTTP/1.1 431 Request Header Fields Too Large");
		CHECK_EQ(anAnswers[7], "");		// closed without a word
		CHECK_EQ(StatusLine(anAnswers[8]), "HTTP/1.1 200 OK");
		CHECK(anAnswers[8].find("Done. You can close this tab") != std::string::npos);
		CHECK(anAnswers[8].find("Cache-Control: no-store\r\n") != std::string::npos);
		size_t aBodyAt = anAnswers[8].find("\r\n\r\n");
		CHECK(aBodyAt != std::string::npos &&
			anAnswers[8].find(std::format("Content-Length: {}\r\n", anAnswers[8].size() - aBodyAt - 4)) != std::string::npos);
	}

	// done: nothing listens any more
	Socket aLate = Connect(AF_INET, aPort);
	CHECK(aLate == NO_SOCKET);
	if (aLate != NO_SOCKET)
		CloseSocket(aLate);
	CHECK(!aListener->Wait(STATE, aQuery, [] { return true; }, anError));
	CHECK(!anError.empty());

	// the player said no
	std::unique_ptr<RedirectListener> aSecond = MakeLoopbackListener();
	CHECK(aSecond->Open(anError));
	std::string aRefusal;
	std::thread aRefusingBrowser([&, aSecondPort = aSecond->Port()]
	{
		aRefusal = Exchange(AF_INET, aSecondPort, { std::format("GET /?error=access_denied&state={} HTTP/1.1\r\n\r\n", STATE) });
	});
	aDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	CHECK(aSecond->Wait(STATE, aQuery, [&] { return std::chrono::steady_clock::now() < aDeadline; }, anError));
	aSecond->Reply(RedirectListener::Outcome::DENIED);
	aRefusingBrowser.join();
	CHECK_EQ(aQuery["error"], "access_denied");
	CHECK_EQ(StatusLine(aRefusal), "HTTP/1.1 200 OK");
	CHECK(aRefusal.find("was not given access") != std::string::npos);

	// a cancelled wait answers nothing; a listener that goes away before Reply tells its browser that it failed
	std::unique_ptr<RedirectListener> aThird = MakeLoopbackListener();
	CHECK(aThird->Open(anError));
	int anAsks = 0;
	CHECK(!aThird->Wait(STATE, aQuery, [&] { return ++anAsks < 3; }, anError));
	CHECK_EQ(anError, "");
	CHECK_EQ(anAsks, 3);
	std::string aLeft;
	std::thread aLeftBrowser([&, aThirdPort = aThird->Port()]
	{
		aLeft = Exchange(AF_INET, aThirdPort, { std::format("GET /?code=c&state={} HTTP/1.1\r\n\r\n", STATE) });
	});
	aDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	CHECK(aThird->Wait(STATE, aQuery, [&] { return std::chrono::steady_clock::now() < aDeadline; }, anError));
	aThird.reset();
	aLeftBrowser.join();
	CHECK_EQ(StatusLine(aLeft), "HTTP/1.1 200 OK");
	CHECK(aLeft.find("did not go through") != std::string::npos);
}

// libcurl gives a request up once the stop function says so, as Stop Sign-In does with the token request. A listener
// plays the server: it answers a request to /x at once and keeps the one with the state of a sign-in waiting.
void TestCurlStop()
{
	GlobalInit();
	std::unique_ptr<RedirectListener> aListener = MakeLoopbackListener();
	std::string anError;
	if (!aListener->Open(anError))
	{
		NotListening(__LINE__, anError);
		return;
	}
	// straight to the loopback address, also where a proxy is set for everything else
#ifdef _WIN32
	_putenv_s("NO_PROXY", "127.0.0.1,localhost");
#else
	setenv("no_proxy", "127.0.0.1,localhost", 1);
	setenv("NO_PROXY", "127.0.0.1,localhost", 1);
#endif

	const std::string STATE = "curl-stop-state";
	std::atomic<bool> aServing{ true };
	bool anAnswered = false;
	Query aQuery;
	std::string aWaitError;
	std::thread aServer([&]
	{
		auto aDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		anAnswered = aListener->Wait(STATE, aQuery, [&] { return aServing && std::chrono::steady_clock::now() < aDeadline; }, aWaitError);
	});

	std::string aBase = std::format("http://127.0.0.1:{}", aListener->Port());
	HttpRequest aRequest;
	aRequest.mUrl = aBase + "/x";
	HttpResponse aResponse = MakeHttpTransport([] { return false; })->Request(aRequest);
	CHECK_EQ(aResponse.mStatus, 404L);		// it does reach the listener
	CHECK_EQ(aResponse.mError, "");

	auto aStart = std::chrono::steady_clock::now();
	auto aStop = [&] { return std::chrono::steady_clock::now() - aStart >= std::chrono::milliseconds(300); };
	aRequest.mUrl = std::format("{}/?state={}&code=c", aBase, STATE);
	aResponse = MakeHttpTransport(aStop)->Request(aRequest);
	auto aTook = std::chrono::steady_clock::now() - aStart;
	CHECK_EQ(aResponse.mStatus, 0L);
	CHECK_EQ(aResponse.mError, "stopped");
	CHECK(aTook >= std::chrono::milliseconds(300));
	CHECK(aTook < std::chrono::seconds(3));		// asked about once a second, not only at the 30 second time limit

	aServing = false;
	aServer.join();
	CHECK(anAnswered);
	CHECK_EQ(aQuery["code"], "c");
	CHECK_EQ(aWaitError, "");
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
	CHECK_EQ(aSaved.mClient, "onedrive-client");
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

	// a sign-in saved while a sync refreshes the one it replaces: that refresh does not save over it
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	aServer.Before([](const HttpRequest& theRequest) { return theRequest.mUrl == "https://login.microsoftonline.com/common/oauth2/v2.0/token"; },
		[&] { a.SignIn(Provider::ONEDRIVE, "m-new-sign-in", "drive-42/approot-id"); });
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(LoadToken(a.mState, Provider::ONEDRIVE, aSaved));
	CHECK_EQ(aSaved.mRefresh, "m-new-sign-in");

	// a refresh token that could not be saved is saved with the next refresh
	a.SignIn(Provider::ONEDRIVE, aServer.IssueRefresh(), "drive-42/approot-id");
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	fs::path aBlocked = TokenPath(a.mState, Provider::ONEDRIVE);
	aBlocked += ".cloudtmp";
	fs::create_directories(aBlocked);	// where the token file is written first: the next save fails
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(LoadToken(a.mState, Provider::ONEDRIVE, aSaved));
	CHECK(!aServer.mRefreshTokens.contains(aSaved.mRefresh));	// the one used up; the new one is only in memory
	fs::remove(aBlocked);
	aServer.mAccessTokens.clear();	// so the next request needs another refresh
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(LoadToken(a.mState, Provider::ONEDRIVE, aSaved));
	CHECK(aServer.mRefreshTokens.contains(aSaved.mRefresh));
	CHECK_EQ(aSaved.mClient, "onedrive-client");

	// or, when no other refresh comes, as the session ends (the game ends one before it starts the next)
	fs::create_directories(aBlocked);
	aServer.mAccessTokens.clear();
	CHECK_EQ(a.Sync(), Result::OK);
	fs::remove(aBlocked);
	a.mEngine.reset();
	CHECK(LoadToken(a.mState, Provider::ONEDRIVE, aSaved));
	CHECK(aServer.mRefreshTokens.contains(aSaved.mRefresh));
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	CHECK_EQ(a.Sync(), Result::OK);
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
	// a session turned down stays until it expires, with what was uploaded to it, unless it is cancelled
	CHECK_EQ(aServer.Count("DELETE https://up.example/"), 1);
	CHECK(aServer.mSessions.empty());
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

	// a 409 for something else than a name taken is no race: nothing to ask the player about, and the next sync goes on
	a.Write("user1.dat", "a4");
	aServer.Instead(IsSession, Answer(409, R"({"error":{"code":"itemNotFound","message":"The parent folder does not exist"}})"));
	CHECK_EQ(a.Sync(), Result::FAILED);
	CHECK_EQ(a.Error(), "uploading the cloud manifest failed: The parent folder does not exist");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "a4");

	// a version changed since, given as a 409 with resourceModified rather than a 412: the race is lost all the same
	a.Write("user1.dat", "a5");
	b.Write("user1.dat", "b5");
	aServer.Before(IsSession, [&]
	{
		CHECK_EQ(b.Sync(), Result::OK);
		aServer.Instead(IsSession, Answer(409, R"({"error":{"code":"resourceModified","message":"The resource has changed since the caller last read it"}})"));
	});
	CHECK_EQ(a.Sync(), Result::CONFLICT);
	CHECK_EQ(a.UseCloud(), Result::OK);
	CHECK_EQ(a.Read("user1.dat"), "b5");

	// the upload is answered as though more of the file were to come: the commit failed, and the session is cancelled
	a.Write("user1.dat", "a6");
	aServer.Instead(IsUpload, Answer(202, R"({"expirationDateTime":"2026-11-15T10:00:00Z","nextExpectedRanges":["0-"]})"));
	CHECK_EQ(a.Sync(), Result::FAILED);
	CHECK_EQ(a.Error(), "uploading the cloud manifest failed: OneDrive did not take the whole file");
	CHECK_EQ(aServer.Count("DELETE https://up.example/"), 2);
	CHECK(aServer.mSessions.empty());
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK_EQ(b.Sync(), Result::OK);
	CHECK_EQ(b.Read("user1.dat"), "a6");

	// making the cloud folder: a 409 means that it is there already only with nameAlreadyExists
	a.Start(OneDriveConfig(), MakeOneDriveStore(OneDriveConfig(), a.mState, LinkTo(aServer)));
	a.Write("user1.dat", "a7");
	aServer.Instead([](const HttpRequest& theRequest) { return theRequest.mUrl.ends_with("/children"); },
		Answer(409, R"({"error":{"code":"notAllowed","message":"The app folder cannot be changed right now"}})"));
	CHECK_EQ(a.Sync(), Result::FAILED);
	CHECK_EQ(a.Error(), "creating the cloud folder failed: The app folder cannot be changed right now");
	CHECK_EQ(a.Sync(), Result::OK);
	CHECK(aServer.mSessions.empty());
	CHECK_EQ(aServer.mTokensOnPreAuthorized, 0);	// nor do the cancels carry an access token

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
#ifndef _WIN32
	std::signal(SIGPIPE, SIG_IGN);	// a write to a connection the listener already closed must fail, not end the tests
#endif

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
		{ "pkce", TestPkce },
		{ "loopback listener", TestLoopback },
		{ "curl stop", TestCurlStop },
		{ "google sign-in", TestGoogleSignIn },
		{ "google sync", TestGoogleSync },
		{ "google conditions", TestGoogleConditions },
		{ "google tokens", TestGoogleTokens },
		{ "onedrive sign-in", TestOneDriveSignIn },
		{ "onedrive sync", TestOneDriveSync },
		{ "onedrive conflicts", TestOneDriveConflicts },
		{ "store ids", TestStoreIds },
	};
	int aSkipped = 0;
	for (const auto& [aName, aTest] : TESTS)
	{
		gTest = aName;
		gSkipped.clear();
		int aBefore = gFailures;
		aTest();
		aSkipped += !gSkipped.empty();
		std::cout << std::format("{:<28} {}\n", aName, gFailures != aBefore ? "FAILED" : gSkipped.empty() ? "ok" : "skipped: " + gSkipped);
	}

	std::error_code anError;
	fs::remove_all(gRoot, anError);
	std::cout << std::format("{} checks, {} failed, {} tests skipped\n", gChecks, gFailures, aSkipped);
	return gFailures == 0 ? 0 : 1;
}
