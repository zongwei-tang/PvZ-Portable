/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSYNC_H__
#define __CLOUDSYNC_H__

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Game-agnostic sync of the userdata folder with a WebDAV server, Google Drive or OneDrive. The whole folder is one
// snapshot: users.dat maps profile names to per-device ids, so mixing files from two devices would cross-wire profiles.
//
// Layout in the cloud folder (<url>/<folder>/ on WebDAV, the app's own hidden folder on Google Drive, Apps/<app>/<folder>/
// on OneDrive):
//   manifest.txt              the committed snapshot: revision, device, summary and a hash per file
//   files/<name>.<hash>       file contents, addressed by hash so an upload never overwrites a file
//                             another device's manifest still points at
// The manifest is written last and only if it did not change since it was read, so it is the commit point of a push.
namespace CloudSync
{

enum class Provider
{
	WEBDAV,
	GOOGLE_DRIVE,
	ONEDRIVE,
};
constexpr int NUM_PROVIDERS = 3;

const char*	ProviderKey(Provider theProvider);		// as written in cloudsave.ini
const char*	ProviderName(Provider theProvider);		// as shown to the player
bool		IsOAuth(Provider theProvider);			// signs in through the browser instead of a password

struct Config
{
	bool			mEnabled = false;
	Provider		mProvider = Provider::WEBDAV;
	std::string		mUrl;					// WebDAV only
	std::string		mUsername;
	std::string		mPassword;
	std::string		mFolder = "PvZPortable";
	std::string		mGoogleClientId;		// empty: the one this build was made with, if any
	std::string		mGoogleClientSecret;
	std::string		mOneDriveClientId;
};

bool		LoadConfig(const std::filesystem::path& thePath, Config& theConfig);	// false if the file is missing
bool		WriteConfig(const std::filesystem::path& thePath, const Config& theConfig);
bool		WriteConfigTemplate(const std::filesystem::path& thePath);
std::string	GoogleClientId(const Config& theConfig);
std::string	GoogleClientSecret(const Config& theConfig);
std::string	OneDriveClientId(const Config& theConfig);
std::string	ClientId(const Config& theConfig);		// the one of theConfig.mProvider; empty for WebDAV
bool		HasBuiltInClientId(Provider theProvider);

bool		IsSyncedFileName(std::string_view theName);
uint64_t	HashBytes(std::string_view theData);
std::string	UrlEscape(std::string_view theText);	// percent-encodes all but the unreserved characters
std::string	UrlEscapePath(std::string_view thePath);	// the same for each segment of a/b/c, keeping the slashes

struct FileEntry
{
	uint64_t	mSize = 0;
	uint64_t	mHash = 0;

	bool operator==(const FileEntry&) const = default;
};

using FileMap = std::map<std::string, FileEntry>;
using Snapshot = std::map<std::string, std::string>;	// file name -> contents

struct Manifest
{
	uint64_t		mRev = 0;
	int64_t			mTime = 0;
	std::string		mDevice;
	std::string		mSummary;
	FileMap			mFiles;
};

inline constexpr const char* MANIFEST_PATH = "manifest.txt";

std::string	WriteManifest(const Manifest& theManifest);
bool		ParseManifest(std::string_view theText, Manifest& theManifest);
FileMap		DescribeSnapshot(const Snapshot& theSnapshot);
bool		SameCommit(const Manifest& theA, const Manifest& theB);

enum class Action
{
	NONE,
	PUSH,
	PULL,
	CONFLICT,
};

// theBase is the manifest of the last successful sync on this device (nullptr if it never synced).
Action		Decide(const Manifest* theBase, const FileMap& theLocal, const Manifest* theRemote);

enum class Result
{
	OK,
	DISABLED,
	OFFLINE,		// the server could not be reached; try again later
	FAILED,			// the server answered with an error
	CONFLICT,		// both sides changed since the last sync; see GetConflict()
	PULL_NEEDED,	// only the cloud changed, but this call was not allowed to touch local files
	PUSH_NEEDED,	// only this device changed, but this call was not allowed to upload
};

const char*	ResultName(Result theResult);

struct HttpRequest
{
	std::string					mMethod = "GET";
	std::string					mUrl;
	const std::string*			mBody = nullptr;
	std::string					mContentType;		// empty: application/octet-stream when there is a body
	std::vector<std::string>	mHeaders;
	std::string					mUsername;			// HTTP basic authentication, when not empty
	std::string					mPassword;
	std::string					mBearer;			// OAuth access token, when not empty
	bool						mFollowRedirects = false;
};

struct HttpResponse
{
	long		mStatus = 0;	// 0 when the request never got an answer
	std::string	mBody;
	std::string	mETag;
	std::string	mError;
};

// Everything goes through this, so tests can swap in in-memory servers.
class Transport
{
public:
	virtual ~Transport() = default;
	virtual HttpResponse	Request(const HttpRequest& theRequest) = 0;
};

// theStop, when given, is asked about once a second while a request runs: true gives it up, as if offline.
std::unique_ptr<Transport>	MakeCurlTransport(std::function<bool()> theStop = nullptr);

// One cloud folder. Paths are relative to it ("manifest.txt", "files/user1.dat.0123456789abcdef").
class Store
{
public:
	enum class Condition
	{
		ANY,		// write whatever is there
		ABSENT,		// only create: CONFLICT if the file exists
		VERSION,	// only replace the version Get() returned: CONFLICT if it changed
	};

	virtual ~Store() = default;

	// Names the cloud folder and account, so the sync state of another one is never mixed up with it.
	virtual std::string	Id() const = 0;
	// theFound is false (and the result OK) when there is no such file.
	virtual Result		Get(const std::string& thePath, bool& theFound, std::string& theBody, std::string& theVersion) = 0;
	virtual Result		Put(const std::string& thePath, const std::string& theBody, Condition theCondition = Condition::ANY,
							const std::string& theVersion = std::string()) = 0;
	virtual Result		Delete(const std::string& thePath) = 0;
	virtual Result		Prepare() { return Result::OK; }	// before the first upload, e.g. to create folders

	const std::string&	GetError() const { return mError; }

protected:
	Result				Fail(Result theResult, std::string theError);

	std::string			mError;
};

std::string	DescribePath(const std::string& thePath);	// "the cloud manifest" or the save file name, for messages

std::unique_ptr<Store>	MakeWebDavStore(const Config& theConfig, std::unique_ptr<Transport> theTransport);
std::unique_ptr<Store>	MakeGoogleDriveStore(const Config& theConfig, const std::filesystem::path& theStateDir, std::unique_ptr<Transport> theTransport);
std::unique_ptr<Store>	MakeOneDriveStore(const Config& theConfig, const std::filesystem::path& theStateDir, std::unique_ptr<Transport> theTransport);
// The store for theConfig.mProvider, talking through libcurl unless theTransport is given.
std::unique_ptr<Store>	MakeStore(const Config& theConfig, const std::filesystem::path& theStateDir, std::unique_ptr<Transport> theTransport = nullptr);

// Sign-in of an OAuth provider, kept in its own file in the state folder (not in cloudsave.ini, which the
// settings dialog rewrites): the refresh token changes while syncing, on the sync thread.
struct Token
{
	std::string	mRefresh;
	std::string	mAccount;		// stable id of the account, so signing in to another one starts a new sync history
	std::string	mAccountName;	// shown to the player, may be empty
	std::string	mClient;		// the client ID it was issued to; empty in files from before it was kept
};

// The token files are read and written under one lock: a new sign-in is saved on its own thread while the sync
// thread may be saving the refresh token of the one it replaces.
std::filesystem::path	TokenPath(const std::filesystem::path& theStateDir, Provider theProvider);
bool		LoadToken(const std::filesystem::path& theStateDir, Provider theProvider, Token& theToken);
// The sign-in theConfig can use: one to theConfig.mProvider issued to the client ID theConfig names, or to a client
// not kept. A refresh token is no good with another client.
bool		LoadTokenFor(const std::filesystem::path& theStateDir, const Config& theConfig, Token& theToken);
bool		SaveToken(const std::filesystem::path& theStateDir, Provider theProvider, const Token& theToken);
// Saves theToken only over a file that still holds the refresh token theOld, so a refresh never undoes a sign-in or
// sign-out made meanwhile.
bool		ReplaceToken(const std::filesystem::path& theStateDir, Provider theProvider, const std::string& theOld, const Token& theToken);
void		DeleteToken(const std::filesystem::path& theStateDir, Provider theProvider);

using Query = std::map<std::string, std::string>;

// Where the browser comes back to after the player signed in: a small HTTP listener on the loopback interface
// (RFC 8252 section 7.3). Tests swap in their own.
class RedirectListener
{
public:
	// what the browser is told in the end
	enum class Outcome
	{
		DONE,		// signed in
		DENIED,		// the player did not give the game access
		FAILED,		// the game says why
	};

	virtual ~RedirectListener() = default;

	// Starts listening on a free port of the loopback interface.
	virtual bool	Open(std::string& theError) = 0;
	virtual int		Port() const = 0;
	// Waits for a request to "/" that carries a code or an error and whose state is theState, stops listening and
	// returns its query; that browser then waits for Reply. The other requests it reads are answered with an error
	// page (a TLS handshake is just closed), and connections still open when it stops listening are closed unanswered.
	// Returns false with theError empty once theKeepWaiting, asked about ten times a second, returns false.
	virtual bool	Wait(const std::string& theState, Query& theQuery, const std::function<bool()>& theKeepWaiting,
						std::string& theError) = 0;
	// Answers the browser Wait returned for with a page about theOutcome. A listener that goes away first answers
	// it with FAILED.
	virtual void	Reply(Outcome theOutcome) = 0;
};

std::unique_ptr<RedirectListener>	MakeLoopbackListener();

// OAuth sign-in to theConfig.mProvider in the player's browser: the authorization code flow with PKCE (RFC 7636),
// answered at a loopback address.
class BrowserSignIn
{
public:
	explicit BrowserSignIn(const Config& theConfig, std::unique_ptr<RedirectListener> theListener = nullptr);
	~BrowserSignIn();

	BrowserSignIn(const BrowserSignIn&) = delete;
	BrowserSignIn& operator=(const BrowserSignIn&) = delete;

	// Gets ready for the answer and makes the address of the provider's sign-in page, for the browser to open.
	Result		Start(std::string& thePage, std::string& theError);
	// Blocks until the browser came back and the sign-in was fetched, the player declined, theKeepWaiting returned
	// false or theTimeLimit ran out. theOnAnswer is called once the browser came back, before the sign-in is fetched,
	// and theOnSignedIn with the sign-in, to keep it, before the browser is told that it is done: false from it fails
	// the sign-in. The browser is told how it went at the end, and only an OK tells it the sign-in is done: keep that
	// one even if theKeepWaiting has turned false since.
	Result		Finish(Transport& theHttp, const std::function<bool()>& theKeepWaiting, const std::function<void()>& theOnAnswer,
					const std::function<bool(const Token&)>& theOnSignedIn, Token& theToken, std::string& theError,
					std::chrono::seconds theTimeLimit = std::chrono::minutes(10));

private:
	Config								mConfig;
	std::unique_ptr<RedirectListener>	mListener;
	std::string							mVerifier;
	std::string							mState;
	std::string							mRedirect;
};

std::string	Sha256(std::string_view theData);		// the 32 bytes of the digest
std::string	Base64Url(std::string_view theData);	// RFC 4648 section 5, without padding

struct ConflictInfo
{
	Manifest	mRemote;
	int64_t		mLocalTime = 0;		// newest modification time among the local files
};

class Engine
{
public:
	// theDataDir is the synced folder, theStateDir keeps the sync state, sign-ins and backups (it must not be inside theDataDir).
	Engine(const Config& theConfig, std::filesystem::path theDataDir, std::filesystem::path theStateDir, std::string theDevice,
		std::unique_ptr<Store> theStore = nullptr);

	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	Snapshot	ReadLocal() const;
	int64_t		LocalTime() const;

	// Pushes or pulls whatever is needed. Without theAllowPull it never writes local files and returns PULL_NEEDED instead,
	// without theAllowPush it never uploads and returns PUSH_NEEDED instead.
	Result		Sync(const Snapshot& theLocal, const std::string& theSummary, bool theAllowPull, bool theAllowPush = true);
	// Conflict resolution: the losing side is copied into a backup folder first.
	Result		KeepLocal(const Snapshot& theLocal, const std::string& theSummary);
	Result		UseCloud();

	const ConflictInfo&	GetConflict() const { return mConflict; }
	const std::string&	GetLastError() const { return mLastError; }
	std::filesystem::path	GetLastBackup() const { return mLastBackup; }

private:
	struct Remote
	{
		bool		mExists = false;
		std::string	mVersion;
		Manifest	mManifest;
	};

	Result		FetchRemote(Remote& theRemote);
	Result		Push(const Snapshot& theLocal, const std::string& theSummary, const Remote& theRemote, bool theUploadAll = false);
	Result		Pull(const Remote& theRemote, const Snapshot& theLocal, bool theBackupLocal);
	// theSkipBroken leaves out files that are missing or damaged in the cloud instead of failing
	Result		Download(const Manifest& theManifest, const Snapshot& theHave, Snapshot& theOut, bool theSkipBroken = false);
	bool		LoadBase(Manifest& theBase) const;
	void		SaveBase(const Manifest& theBase) const;
	void		Backup(const Snapshot& theSnapshot, std::string_view theSide);
	Result		Fail(Result theResult, std::string theError);
	Result		StoreFailed(Result theResult);

	Config					mConfig;
	std::filesystem::path	mDataDir;
	std::filesystem::path	mStateDir;
	std::string				mDevice;
	std::unique_ptr<Store>	mStore;
	ConflictInfo			mConflict;
	std::string				mLastError;
	std::filesystem::path	mLastBackup;
};

void		GlobalInit();	// call once on the main thread before any Engine or Transport is used on another thread

}

#endif
