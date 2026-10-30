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
#include <memory>
#include <thread>
#include <SDL.h>
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

std::unique_ptr<CloudSync::Engine>	gEngine;
std::thread							gWorker;
std::atomic<bool>					gWorkerDone{ false };
CloudSync::Result					gWorkerResult = CloudSync::Result::OK;
std::string							gWorkerSignature;

Pending			gPending = Pending::NONE;
std::string		gSyncedSignature;		// the userdata folder as of the last sync that left both sides equal
std::string		gSeenSignature;
Clock::time_point	gSeenAt;
Clock::time_point	gNextCheck;
Clock::time_point	gNextRemoteCheck;
Clock::time_point	gRetryAt;

fs::path DataDir()
{
	return Sexy::PathFromU8(Sexy::GetAppDataPath("userdata"));
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

void LogResult(const char* theWhat, CloudSync::Result theResult)
{
	if (theResult == CloudSync::Result::OK)
		PvzpLogLn("cloud save: {} ok", theWhat);
	else
		PvzpLogLn("cloud save: {} {}: {}", theWhat, CloudSync::ResultName(theResult), gEngine->GetLastError());
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
	if (!gWorker.joinable())
		return;
	gWorker.join();
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
	gWorker = std::thread([aSnapshot = std::move(aSnapshot), aSummary = std::move(aSummary)]()
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

	LawnDialog* aDialog = static_cast<LawnDialog*>(gLawnApp->DoDialog(
		Dialogs::DIALOG_CLOUD_SAVE_CONFLICT, true, "CLOUD SAVE CONFLICT", aLines, "", Sexy::Dialog::BUTTONS_YES_NO));
	if (aDialog->mLawnYesButton)
		aDialog->mLawnYesButton->mLabel = "Use Cloud";
	if (aDialog->mLawnNoButton)
		aDialog->mLawnNoButton->mLabel = "Keep This";
	gLawnApp->mWidgetManager->SetFocus(aDialog);
}

}

void Init()
{
	fs::path aConfigPath = Sexy::PathFromU8(Sexy::GetAppDataPath("cloudsave.ini"));
	CloudSync::Config aConfig;
	if (!CloudSync::LoadConfig(aConfigPath, aConfig))
	{
		CloudSync::WriteConfigTemplate(aConfigPath);
		return;
	}
	if (!aConfig.mEnabled)
		return;

	CloudSync::GlobalInit();
	gEngine = std::make_unique<CloudSync::Engine>(aConfig, DataDir(), Sexy::PathFromU8(Sexy::GetAppDataPath(".")), DeviceName());

	// The profiles are not loaded yet, so a newer cloud save can simply replace the files.
	// Uploading waits for Update(), when the summary of the loaded profile is known.
	std::string aSignature = Signature();
	CloudSync::Result aResult = gEngine->Sync(gEngine->ReadLocal(), "", true, false);
	LogResult("startup sync", aResult);
	HandleResult(aResult, aResult == CloudSync::Result::OK ? Signature() : aSignature);

	Clock::time_point aNow = Clock::now();
	gNextCheck = aNow;
	gNextRemoteCheck = aNow + REMOTE_INTERVAL;
}

void Update()
{
	if (!gEngine)
		return;

	if (gWorker.joinable())
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

void Shutdown()
{
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
	gEngine.reset();
}

}

#endif
