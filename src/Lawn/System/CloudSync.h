/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSYNC_H__
#define __CLOUDSYNC_H__

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Game-agnostic WebDAV sync of the userdata folder. The whole folder is one snapshot: users.dat maps
// profile names to per-device ids, so mixing files from two devices would cross-wire profiles.
//
// Remote layout under <url>/<folder>/:
//   manifest.txt              the committed snapshot: revision, device, summary and a hash per file
//   files/<name>.<hash>       file contents, addressed by hash so an upload never overwrites a file
//                             another device's manifest still points at
// The manifest is written last with If-Match, so it is the commit point of a push.
namespace CloudSync
{

struct Config
{
	bool			mEnabled = false;
	std::string		mUrl;
	std::string		mUsername;
	std::string		mPassword;
	std::string		mFolder = "PvZPortable";
};

bool		LoadConfig(const std::filesystem::path& thePath, Config& theConfig);	// false if the file is missing
bool		WriteConfigTemplate(const std::filesystem::path& thePath);
bool		IsSyncedFileName(std::string_view theName);
uint64_t	HashBytes(std::string_view theData);

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

std::string	WriteManifest(const Manifest& theManifest);
bool		ParseManifest(std::string_view theText, Manifest& theManifest);
FileMap		DescribeSnapshot(const Snapshot& theSnapshot);

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

struct HttpResponse
{
	long		mStatus = 0;	// 0 when the request never got an answer
	std::string	mBody;
	std::string	mETag;
	std::string	mError;
};

// The engine talks to the server only through this, so tests can swap in an in-memory server.
class Transport
{
public:
	virtual ~Transport() = default;
	virtual HttpResponse	Request(const char* theMethod, const std::string& theUrl, const std::string* theBody, const std::vector<std::string>& theHeaders) = 0;
};

std::unique_ptr<Transport>	MakeCurlTransport(const Config& theConfig);

struct ConflictInfo
{
	Manifest	mRemote;
	int64_t		mLocalTime = 0;		// newest modification time among the local files
};

class Engine
{
public:
	// theDataDir is the synced folder, theStateDir keeps the sync state and backups (it must not be inside theDataDir).
	Engine(const Config& theConfig, std::filesystem::path theDataDir, std::filesystem::path theStateDir, std::string theDevice,
		std::unique_ptr<Transport> theTransport = nullptr);

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
		std::string	mETag;
		Manifest	mManifest;
	};

	Result		FetchRemote(Remote& theRemote);
	Result		Push(const Snapshot& theLocal, const std::string& theSummary, const Remote& theRemote);
	Result		Pull(const Remote& theRemote, const Snapshot& theLocal, bool theBackupLocal);
	Result		Download(const Manifest& theManifest, const Snapshot& theHave, Snapshot& theOut);
	Result		EnsureFolders();
	bool		LoadBase(Manifest& theBase) const;
	void		SaveBase(const Manifest& theBase) const;
	void		Backup(const Snapshot& theSnapshot, std::string_view theSide);
	Result		Fail(Result theResult, std::string theError);

	std::string	FolderPath() const;
	std::string	BlobPath(const std::string& theName, uint64_t theHash) const;

	Config					mConfig;
	std::filesystem::path	mDataDir;
	std::filesystem::path	mStateDir;
	std::string				mDevice;
	std::unique_ptr<Transport>	mHttp;
	ConflictInfo			mConflict;
	std::string				mLastError;
	std::filesystem::path	mLastBackup;
	bool					mFoldersReady = false;
};

void		GlobalInit();	// call once on the main thread before any Engine is used on another thread

}

#endif
