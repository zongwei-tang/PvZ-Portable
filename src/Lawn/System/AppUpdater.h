/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local app updater patch).
 */

#ifndef __APPUPDATER_H__
#define __APPUPDATER_H__

// Updates of the app, behind Check Updates in the options of the main menu.
//
// Mac (PVZP_MAC_UPDATER, MacUpdater.mm): Sparkle (https://sparkle-project.org). The app built by macos/build-app.sh
// carries Sparkle.framework and the update feed in its Info.plist; Sparkle is loaded at run time, so the game itself
// builds without it, and an app without it (or a bare binary) simply has no updater.
//
// iOS (PVZP_IOS_UPDATER, IosUpdater.mm): an app cannot replace itself there. A sideloaded one is updated by AltStore or
// SideStore from the AltStore source of the releases, so the game only looks at that source and, when there is a newer
// build, says so and opens AltStore or SideStore.
//
// Other systems get no-op stubs.
namespace AppUpdater
{

#if defined(PVZP_MAC_UPDATER) || defined(PVZP_IOS_UPDATER)
void	Start();		// on the main thread once the window is up: looks for updates in the background
bool	Available();	// there is an updater, so CheckNow() does something
void	CheckNow();		// shows whether there is an update, and offers it
#else
inline void	Start() {}
inline bool	Available() { return false; }
inline void	CheckNow() {}
#endif

}

#endif
