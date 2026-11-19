/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifdef PVZP_CLOUD_SAVE

#include "CloudSave.h"
#include "CloudSync.h"
#include "PlayerInfo.h"
#include "ProfileMgr.h"
#include "../Widget/GameSelector.h"
#include "../Widget/LawnDialog.h"
#include "../../LawnApp.h"
#include "../../PvzpLib/PvzpDebug.h"
#include "../../SexyAppFramework/widget/WidgetManager.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <format>
#include <mutex>
#include <thread>
#include <SDL3/SDL.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace CloudSave
{

namespace
{

constexpr auto CHECK_INTERVAL = std::chrono::seconds(2);		// how often the userdata folder is looked at
constexpr auto SETTLE_TIME = std::chrono::seconds(3);			// a change must stay put this long before it is uploaded
constexpr auto REMOTE_INTERVAL = std::chrono::minutes(5);		// how often the cloud is checked for saves from other devices
constexpr auto RETRY_INTERVAL = std::chrono::minutes(1);		// wait after the server could not be reached

enum class Pending
{
	NONE,
	PULL,		// only the cloud changed: load it the next time the main menu is idle
	CONFLICT,	// both changed: ask the player on the main menu
};

// Plain pointers: an exit() that skips Shutdown() must not run a std::thread destructor (std::terminate on a
// joinable thread) or free the engine under a still running worker.
CloudSync::Engine*					gEngine = nullptr;
std::thread*						gWorker = nullptr;
std::atomic<bool>					gWorkerDone{ false };
CloudSync::Result					gWorkerResult = CloudSync::Result::OK;
std::string							gWorkerSignature;

CloudSync::Config	gConfig;
Pending			gPending = Pending::NONE;
std::string		gStatus;				// what the settings dialog shows about the last sync
std::string		gSyncedSignature;		// the userdata folder as of the last sync that left both sides equal
std::string		gSeenSignature;
Clock::time_point	gSeenAt;
Clock::time_point	gNextCheck;
Clock::time_point	gNextRemoteCheck;
Clock::time_point	gRetryAt;

// Browser sign-in, on its own thread. The thread saves the sign-in before the browser is told that it is done, and
// fills in gSignIn and the results; joining it and restarting the engine happen on the main thread (FinishSignIn).
std::thread*						gSignInThread = nullptr;
std::atomic<bool>					gSignInCancel{ false };
std::atomic<bool>					gSignInDone{ false };
std::mutex							gSignInMutex;
SignInState							gSignIn;				// guarded by gSignInMutex, like the two below
CloudSync::Result					gSignInResult = CloudSync::Result::OK;
std::string							gSignInError;

fs::path DataDir()
{
	return Sexy::PathFromU8(Sexy::GetAppDataPath("userdata"));
}

// sync state, sign-ins and backups
fs::path StateDir()
{
	return Sexy::PathFromU8(Sexy::GetAppDataPath("."));
}

std::string DeviceName()
{
	std::string aHost;
#ifdef _WIN32
	if (const char* aName = std::getenv("COMPUTERNAME"))
		aHost = aName;
#else
	char aName[256] = {};
	if (gethostname(aName, sizeof(aName) - 1) == 0)
		aHost = aName;
#endif
	return aHost.empty() ? std::string(SDL_GetPlatform()) : std::format("{} ({})", SDL_GetPlatform(), aHost);
}

// Cheap fingerprint of the synced files, so the folder is only read and hashed after something was written.
std::string Signature()
{
	std::string aSignature;
	std::error_code anError;
	for (const fs::directory_entry& anEntry : fs::directory_iterator(DataDir(), anError))
	{
		std::string aName = anEntry.path().filename().string();
		if (!anEntry.is_regular_file() || !CloudSync::IsSyncedFileName(aName))
			continue;
		aSignature += std::format("{}:{}:{};", aName, anEntry.file_size(anError), anEntry.last_write_time(anError).time_since_epoch().count());
	}
	return aSignature;
}

std::string Summary()
{
	PlayerInfo* aPlayer = gLawnApp ? gLawnApp->mPlayerInfo : nullptr;
	if (aPlayer == nullptr)
		return "";

	std::string aSummary = std::format("{}: level {}, {}", aPlayer->mName, LawnApp::GetStageString(aPlayer->mLevel), LawnApp::GetMoneyString(aPlayer->mCoins));
	if (aPlayer->mFinishedAdventure > 0)
		aSummary += std::format(", adventure finished {}x", aPlayer->mFinishedAdventure);
	return aSummary;
}

std::string FormatTime(int64_t theTime)
{
	if (theTime <= 0)
		return "unknown time";
	std::time_t aTime = static_cast<std::time_t>(theTime);
	std::tm aTm{};
#ifdef _WIN32
	localtime_s(&aTm, &aTime);
#else
	localtime_r(&aTime, &aTm);
#endif
	char aText[32];
	std::strftime(aText, sizeof(aText), "%Y-%m-%d %H:%M", &aTm);
	return aText;
}

std::string NowText()
{
	return FormatTime(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void LogResult(const char* theWhat, CloudSync::Result theResult)
{
	if (theResult == CloudSync::Result::OK)
		PvzpLogLn("cloud save: {} ok", theWhat);
	else
		PvzpLogLn("cloud save: {} {}: {}", theWhat, CloudSync::ResultName(theResult), gEngine->GetLastError());

	switch (theResult)
	{
	case CloudSync::Result::OK:				gStatus = std::format("Synced at {}.", NowText()); break;
	case CloudSync::Result::OFFLINE:		gStatus = std::format("Can't reach the server: {}", gEngine->GetLastError()); break;
	case CloudSync::Result::FAILED:			gStatus = std::format("Sync failed: {}", gEngine->GetLastError()); break;
	case CloudSync::Result::CONFLICT:		gStatus = "Both copies changed. You will be asked which one to keep."; break;
	case CloudSync::Result::PULL_NEEDED:	gStatus = "A newer cloud save will be loaded on the main menu."; break;
	case CloudSync::Result::PUSH_NEEDED:	gStatus = "Waiting to upload."; break;
	case CloudSync::Result::DISABLED:		gStatus = "Cloud save is off."; break;
	}
}

// Applies a sync result that was computed against the files of theSignature.
void HandleResult(CloudSync::Result theResult, const std::string& theSignature)
{
	switch (theResult)
	{
	case CloudSync::Result::OK:
		gSyncedSignature = theSignature;
		break;
	case CloudSync::Result::PULL_NEEDED:
		gPending = Pending::PULL;
		break;
	case CloudSync::Result::CONFLICT:
		gPending = Pending::CONFLICT;
		break;
	case CloudSync::Result::PUSH_NEEDED:
		break;	// Update() uploads it once the summary of the loaded profile is known
	default:
		gRetryAt = Clock::now() + RETRY_INTERVAL;
		break;
	}
}

void JoinWorker()
{
	if (gWorker == nullptr)
		return;
	gWorker->join();
	delete gWorker;
	gWorker = nullptr;
	gWorkerDone = false;
	LogResult("background sync", gWorkerResult);
	HandleResult(gWorkerResult, gWorkerSignature);
}

void StartWorker()
{
	gWorkerSignature = Signature();
	CloudSync::Snapshot aSnapshot = gEngine->ReadLocal();
	std::string aSummary = Summary();
	gWorkerDone = false;
	gWorker = new std::thread([aSnapshot = std::move(aSnapshot), aSummary = std::move(aSummary)]()
	{
		gWorkerResult = gEngine->Sync(aSnapshot, aSummary, false);
		gWorkerDone = true;
	});
}

void ShowMessage(const std::string& theHeader, const std::string& theLines)
{
	gLawnApp->DoDialog(Dialogs::DIALOG_INFO, true, theHeader, theLines, "[DIALOG_BUTTON_OK]", Sexy::Dialog::BUTTONS_FOOTER);
}

void ReloadProfiles()
{
	std::string aName = gLawnApp->mPlayerInfo ? gLawnApp->mPlayerInfo->mName : "";
	gLawnApp->mProfileMgr->Clear();
	gLawnApp->mProfileMgr->Load();
	gLawnApp->mPlayerInfo = gLawnApp->mProfileMgr->GetProfile(aName);
	if (gLawnApp->mPlayerInfo == nullptr)
		gLawnApp->mPlayerInfo = gLawnApp->mProfileMgr->GetAnyProfile();

	gLawnApp->mWidgetManager->MarkAllDirty();
	if (gLawnApp->mPlayerInfo == nullptr)
		gLawnApp->DoCreateUserDialog();
	else if (gLawnApp->mGameSelector)
		gLawnApp->mGameSelector->SyncProfile(true);
}

// Files may only be swapped while nothing is being played and no dialog is up.
bool AtSafePoint()
{
	return gLawnApp->mLoadingThreadCompleted && gLawnApp->mGameScene == GameScenes::SCENE_MENU && gLawnApp->mGameSelector &&
		gLawnApp->mBoard == nullptr && gLawnApp->GetDialogCount() == 0;
}

// Pops up as the player arrives on the main menu, so a key still held from before must not pick a side:
// only a click on one of its buttons answers it.
class ConflictDialog : public LawnDialog
{
public:
	explicit ConflictDialog(const std::string& theLines) :
		LawnDialog(gLawnApp, Dialogs::DIALOG_CLOUD_SAVE_CONFLICT, true, "CLOUD SAVE CONFLICT", theLines, "", Sexy::Dialog::BUTTONS_YES_NO)
	{
	}

	void KeyDown([[maybe_unused]] Sexy::KeyCode theKey) override {}
};

void AskAboutConflict()
{
	const CloudSync::ConflictInfo& aConflict = gEngine->GetConflict();
	std::string aLines = std::format(
		"Your saves changed both on this device and in the cloud.\n\n"
		"This device, {}:\n{}\n\n"
		"Cloud, {} from {}:\n{}\n\n"
		"The copy you don't keep is backed up.",
		FormatTime(aConflict.mLocalTime), Summary(),
		FormatTime(aConflict.mRemote.mTime), aConflict.mRemote.mDevice, aConflict.mRemote.mSummary);

	gLawnApp->KillDialog(Dialogs::DIALOG_CLOUD_SAVE_CONFLICT);
	ConflictDialog* aDialog = new ConflictDialog(aLines);
	LawnApp::CenterDialog(aDialog, aDialog->mWidth, aDialog->mHeight);
	gLawnApp->AddDialog(Dialogs::DIALOG_CLOUD_SAVE_CONFLICT, aDialog);
	if (aDialog->mLawnYesButton)
		aDialog->mLawnYesButton->mLabel = "Use Cloud";
	if (aDialog->mLawnNoButton)
		aDialog->mLawnNoButton->mLabel = "Keep This";
	gLawnApp->mWidgetManager->SetFocus(aDialog);
}

fs::path ConfigPath()
{
	return Sexy::PathFromU8(Sexy::GetAppDataPath("cloudsave.ini"));
}

// The sign-in to the service of theConfig that theConfig can use: one made with the client ID it names.
bool LoadSignIn(const CloudSync::Config& theConfig, CloudSync::Token& theToken)
{
	return CloudSync::IsOAuth(theConfig.mProvider) && CloudSync::LoadTokenFor(StateDir(), theConfig, theToken);
}

// The same with the settings last applied.
bool LoadSignIn(CloudSync::Provider theProvider, CloudSync::Token& theToken)
{
	CloudSync::Config aConfig = gConfig;
	aConfig.mProvider = theProvider;
	return LoadSignIn(aConfig, theToken);
}

// Starts over with theConfig: drops the running engine and anything it was waiting for.
void StartEngine(const CloudSync::Config& theConfig)
{
	JoinWorker();
	delete gEngine;
	gEngine = nullptr;
	gConfig = theConfig;
	gPending = Pending::NONE;
	gSyncedSignature.clear();
	gSeenSignature.clear();
	gStatus = theConfig.mEnabled ? "Not synced yet." : "Cloud save is off.";
	if (!theConfig.mEnabled)
		return;
	if (CloudSync::IsOAuth(theConfig.mProvider) && !IsSignedIn(theConfig.mProvider))
	{
		gStatus = std::format("Sign in to {} to start syncing.", CloudSync::ProviderName(theConfig.mProvider));
		return;
	}

	CloudSync::GlobalInit();
	gEngine = new CloudSync::Engine(theConfig, DataDir(), StateDir(), DeviceName());
	Clock::time_point aNow = Clock::now();
	gNextCheck = aNow;
	gRetryAt = aNow;
	gNextRemoteCheck = aNow + REMOTE_INTERVAL;
}

void StopSignIn()
{
	if (gSignInThread == nullptr)
		return;
	gSignInCancel = true;
	gSignInThread->join();
	delete gSignInThread;
	gSignInThread = nullptr;
}

void FinishSignIn()
{
	if (gSignInThread == nullptr || !gSignInDone)
		return;
	gSignInThread->join();
	delete gSignInThread;
	gSignInThread = nullptr;

	CloudSync::Result aResult;
	std::string anError;
	CloudSync::Provider aProvider;
	{
		std::lock_guard aLock(gSignInMutex);
		aResult = gSignInResult;
		anError = gSignInError;
		aProvider = gSignIn.mProvider;
	}

	SignInState::Phase aPhase = SignInState::Phase::FAILED;
	std::string aMessage;
	if (aResult != CloudSync::Result::OK && gSignInCancel)
	{
		aPhase = SignInState::Phase::IDLE;
		aMessage = "Sign-in stopped.";
	}
	else if (aResult != CloudSync::Result::OK)
	{
		PvzpLogLn("cloud save: signing in to {} {}: {}", CloudSync::ProviderName(aProvider), CloudSync::ResultName(aResult), anError);
		aMessage = std::format("Sign-in failed: {}", anError);
	}
	else
	{
		// Saved by the thread, also when Stop came too late for it, since the browser has said that it is done. A
		// refresh by the engine, still with the sign-in this one replaces, does not save over it (ReplaceToken).
		PvzpLogLn("cloud save: signed in to {}", CloudSync::ProviderName(aProvider));
		StartEngine(gConfig);
		aPhase = SignInState::Phase::DONE;
		aMessage = AccountText(aProvider);
	}

	std::lock_guard aLock(gSignInMutex);
	gSignIn.mPhase = aPhase;
	gSignIn.mMessage = aMessage;
}

}

void Init()
{
	CloudSync::Config aConfig;
	if (!CloudSync::LoadConfig(ConfigPath(), aConfig))
		CloudSync::WriteConfigTemplate(ConfigPath());
	StartEngine(aConfig);
	if (!gEngine)
		return;

	// The profiles are not loaded yet, so a newer cloud save can simply replace the files.
	// Uploading waits for Update(), when the summary of the loaded profile is known.
	std::string aSignature = Signature();
	CloudSync::Result aResult = gEngine->Sync(gEngine->ReadLocal(), "", true, false);
	LogResult("startup sync", aResult);
	HandleResult(aResult, aResult == CloudSync::Result::OK ? Signature() : aSignature);
}

void Update()
{
	FinishSignIn();
	if (!gEngine)
		return;

	if (gWorker)
	{
		if (!gWorkerDone)
			return;
		JoinWorker();
	}

	Clock::time_point aNow = Clock::now();
	if (gPending != Pending::NONE)
	{
		if (!AtSafePoint())
			return;

		if (gPending == Pending::PULL)
		{
			gPending = Pending::NONE;
			std::string aSignature = Signature();
			CloudSync::Result aResult = gEngine->Sync(gEngine->ReadLocal(), Summary(), true);
			LogResult("pull", aResult);
			HandleResult(aResult, aResult == CloudSync::Result::OK ? Signature() : aSignature);
			if (aResult == CloudSync::Result::OK)
				ReloadProfiles();
		}
		if (gPending == Pending::CONFLICT)
			AskAboutConflict();
		return;
	}

	if (aNow < gNextCheck || aNow < gRetryAt)
		return;
	gNextCheck = aNow + CHECK_INTERVAL;

	std::string aSignature = Signature();
	if (aSignature != gSeenSignature)
	{
		gSeenSignature = aSignature;
		gSeenAt = aNow;
	}

	bool aLocalChanged = aSignature != gSyncedSignature && aNow - gSeenAt >= SETTLE_TIME;
	if (aLocalChanged || aNow >= gNextRemoteCheck)
	{
		gNextRemoteCheck = aNow + REMOTE_INTERVAL;
		StartWorker();
	}
}

void ResolveConflict(bool theUseCloud)
{
	gLawnApp->KillDialog(Dialogs::DIALOG_CLOUD_SAVE_CONFLICT);
	if (!gEngine)
		return;

	gPending = Pending::NONE;
	std::string aSignature = Signature();
	CloudSync::Result aResult;
	if (theUseCloud)
	{
		aResult = gEngine->UseCloud();
		LogResult("use cloud", aResult);
	}
	else
	{
		gLawnApp->WriteCurrentUserConfig();
		aSignature = Signature();
		aResult = gEngine->KeepLocal(gEngine->ReadLocal(), Summary());
		LogResult("keep local", aResult);
	}
	HandleResult(aResult, aResult == CloudSync::Result::OK ? Signature() : aSignature);

	if (aResult == CloudSync::Result::OK)
	{
		if (theUseCloud)
			ReloadProfiles();
		ShowMessage("CLOUD SAVE", std::format("{}\n\nThe other copy was backed up to:\n{}",
			theUseCloud ? "The cloud save is loaded." : "This device's save is now in the cloud.",
			Sexy::PathToU8(gEngine->GetLastBackup())));
	}
	else if (aResult != CloudSync::Result::CONFLICT)
	{
		// HandleResult() scheduled a retry, which will notice the conflict again
		ShowMessage("CLOUD SAVE", std::format("The cloud could not be reached, nothing was changed.\n\n{}", gEngine->GetLastError()));
	}
}

CloudSync::Config GetSettings()
{
	return gConfig;
}

bool ApplySettings(const CloudSync::Config& theConfig)
{
	if (!CloudSync::WriteConfig(ConfigPath(), theConfig))
		return false;

	// read back, so the engine sees exactly what the next start will
	CloudSync::Config aConfig;
	CloudSync::LoadConfig(ConfigPath(), aConfig);
	StartEngine(aConfig);
	return true;
}

CloudSync::Result SyncNow()
{
	if (!gEngine)
		return CloudSync::Result::DISABLED;

	JoinWorker();
	gPending = Pending::NONE;
	gLawnApp->WriteCurrentUserConfig();
	std::string aBefore = Signature();
	CloudSync::Result aResult = gEngine->Sync(gEngine->ReadLocal(), Summary(), true);
	LogResult("sync now", aResult);
	std::string anAfter = Signature();
	HandleResult(aResult, aResult == CloudSync::Result::OK ? anAfter : aBefore);
	if (aResult == CloudSync::Result::OK && anAfter != aBefore)
	{
		ReloadProfiles();
		gStatus = std::format("Loaded the cloud save at {}.", NowText());
	}
	return aResult;
}

std::string StatusText()
{
	return gStatus;
}

void Shutdown()
{
	FinishSignIn();		// one the browser has just said is done
	StopSignIn();
	if (!gEngine)
		return;

	JoinWorker();
	// An unanswered conflict stays unresolved until the next start rather than being decided here.
	if (gPending == Pending::NONE)
	{
		std::string aSignature = Signature();
		if (aSignature != gSyncedSignature)
			LogResult("final sync", gEngine->Sync(gEngine->ReadLocal(), Summary(), false));
	}
	delete gEngine;
	gEngine = nullptr;
}

bool StartSignIn(CloudSync::Provider theProvider)
{
	FinishSignIn();
	if (gSignInThread || !CloudSync::IsOAuth(theProvider))
		return false;

	CloudSync::Config aConfig = gConfig;
	aConfig.mProvider = theProvider;
	CloudSync::GlobalInit();

	// Getting ready only means listening on the loopback interface, so it happens here and the page can open at once.
	std::shared_ptr<CloudSync::BrowserSignIn> aSignIn = std::make_shared<CloudSync::BrowserSignIn>(aConfig);
	std::string aPage;
	std::string anError;
	CloudSync::Result aStarted = aSignIn->Start(aPage, anError);
	{
		std::lock_guard aLock(gSignInMutex);
		gSignIn = SignInState();
		gSignIn.mProvider = theProvider;
		if (aStarted != CloudSync::Result::OK)
		{
			PvzpLogLn("cloud save: signing in to {} {}: {}", CloudSync::ProviderName(theProvider), CloudSync::ResultName(aStarted), anError);
			gSignIn.mPhase = SignInState::Phase::FAILED;
			gSignIn.mMessage = std::format("Sign-in failed: {}", anError);
			return true;
		}
		gSignIn.mPhase = SignInState::Phase::WAITING;
		gSignIn.mPage = aPage;
		gSignIn.mMessage = "Sign in and allow access in the browser on this computer. If the page shows an error, check the client settings (see the README).";
	}

	gSignInCancel = false;
	gSignInDone = false;
	gSignInThread = new std::thread([aSignIn, aStateDir = StateDir(), theProvider]()
	{
		// Stop also gives up a request on its way, so the thread is joined within a second
		std::unique_ptr<CloudSync::Transport> aHttp = CloudSync::MakeCurlTransport([]() { return gSignInCancel.load(); });
		CloudSync::Token aToken;
		std::string anError;
		CloudSync::Result aResult = aSignIn->Finish(*aHttp,
			[]() { return !gSignInCancel; },
			[]()
			{
				std::lock_guard aLock(gSignInMutex);
				if (!gSignInCancel)
				{
					gSignIn.mMessage = "Finishing the sign-in...";
					gSignIn.mAnswered = true;
				}
			},
			[&](const CloudSync::Token& theToken)
			{
				// Saved here, before the browser says that it is done, so a game closed right then keeps it. Another
				// program that has the file open (a virus scanner, say) can turn a try down.
				for (int aTry = 0; aTry < 3; aTry++)
				{
					if (aTry > 0)
						std::this_thread::sleep_for(std::chrono::milliseconds(100));
					if (CloudSync::SaveToken(aStateDir, theProvider, theToken))
						return true;
				}
				return false;
			},
			aToken, anError);
		{
			std::lock_guard aLock(gSignInMutex);
			gSignInResult = aResult;
			gSignInError = anError;
		}
		gSignInDone = true;
	});
	return true;
}

void CancelSignIn()
{
	// The thread notices within a tenth of a second, or a second while a request is on its way, and Update() joins
	// it. Until then StartSignIn() refuses to start another one.
	if (gSignInThread == nullptr || gSignInDone)
		return;
	gSignInCancel = true;
	std::lock_guard aLock(gSignInMutex);
	gSignIn.mPhase = SignInState::Phase::IDLE;
	gSignIn.mMessage = "Sign-in stopped.";
}

SignInState GetSignIn()
{
	FinishSignIn();
	std::lock_guard aLock(gSignInMutex);
	return gSignIn;
}

void ClearSignIn()
{
	// DONE and FAILED are only set with no thread running; one that has ended but is not finished yet stays WAITING
	// until then, so its result is still shown the next time
	std::lock_guard aLock(gSignInMutex);
	if (gSignIn.mPhase == SignInState::Phase::DONE || gSignIn.mPhase == SignInState::Phase::FAILED)
	{
		gSignIn.mPhase = SignInState::Phase::IDLE;
		gSignIn.mMessage.clear();
	}
}

bool IsSignedIn(CloudSync::Provider theProvider)
{
	CloudSync::Token aToken;
	return LoadSignIn(theProvider, aToken);
}

bool IsSignedIn(const CloudSync::Config& theConfig)
{
	CloudSync::Token aToken;
	return LoadSignIn(theConfig, aToken);
}

std::string AccountText(CloudSync::Provider theProvider)
{
	const char* aName = CloudSync::ProviderName(theProvider);
	CloudSync::Token aToken;
	if (!LoadSignIn(theProvider, aToken))
		return std::format("Not signed in to {}.", aName);
	if (aToken.mAccountName.empty())
		return std::format("Signed in to {}.", aName);
	return std::format("Signed in to {} as {}.", aName, aToken.mAccountName);
}

void SignOut(CloudSync::Provider theProvider)
{
	JoinWorker();
	CloudSync::DeleteToken(StateDir(), theProvider);
	PvzpLogLn("cloud save: signed out of {}", CloudSync::ProviderName(theProvider));
	StartEngine(gConfig);
}

}

#endif
