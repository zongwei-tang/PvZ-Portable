/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSAVE_H__
#define __CLOUDSAVE_H__

#ifdef PVZP_CLOUD_SAVE
#include "CloudSync.h"
#include <string>
#endif

// Keeps userdata/ in sync with the cloud folder configured in cloudsave.ini: WebDAV, Google Drive or OneDrive
// (see CloudSync.h for the protocol). Builds without libcurl (PVZP_CLOUD_SAVE undefined) get no-op stubs.
namespace CloudSave
{

#ifdef PVZP_CLOUD_SAVE
void	Init();						// before the profiles are loaded: pulls a newer cloud save, or notes a conflict
void	Update();					// every frame: uploads changed saves in the background, asks about conflicts on the main menu
void	Shutdown();					// after the last saves were written: uploads them before the game exits
void	ResolveConflict(bool theUseCloud);

// For the settings dialog, which is only offered on the main menu, where files may be swapped.
CloudSync::Config	GetSettings();
bool				ApplySettings(const CloudSync::Config& theConfig);	// writes cloudsave.ini and restarts syncing with it
CloudSync::Result	SyncNow();		// blocks until done; a conflict is asked about once the dialogs are closed
std::string			StatusText();

// Sign-in to Google Drive or OneDrive in the player's browser. The wait for the browser runs on its own thread.
struct SignInState
{
	enum class Phase
	{
		IDLE,
		WAITING,	// the sign-in page is up to the player, then the sign-in is fetched
		DONE,
		FAILED,
	};

	Phase					mPhase = Phase::IDLE;
	CloudSync::Provider		mProvider = CloudSync::Provider::WEBDAV;
	std::string				mPage;		// the provider's sign-in page, for the browser
	std::string				mMessage;
	bool					mAnswered = false;	// WAITING: the browser came back, the sign-in is being fetched
};

// With the settings last applied. False for a provider without sign-in, or while the last sign-in is still running
// or stopping; otherwise the state is WAITING, for the caller to open mPage in the browser, or FAILED.
bool		StartSignIn(CloudSync::Provider theProvider);
void		CancelSignIn();
SignInState	GetSignIn();									// also finishes a sign-in that just completed
void		ClearSignIn();									// back to IDLE from DONE or FAILED, once the player saw it
bool		IsSignedIn(CloudSync::Provider theProvider);
std::string	AccountText(CloudSync::Provider theProvider);	// "Signed in to OneDrive." and the like
void		SignOut(CloudSync::Provider theProvider);		// forgets the sign-in on this device
#else
inline void	Init() {}
inline void	Update() {}
inline void	Shutdown() {}
inline void	ResolveConflict(bool) {}
#endif

}

#endif
