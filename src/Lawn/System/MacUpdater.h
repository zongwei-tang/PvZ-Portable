/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local Mac app updater patch).
 */

#ifndef __MACUPDATER_H__
#define __MACUPDATER_H__

// Updates of the Mac app through Sparkle (https://sparkle-project.org). The app built by macos/build-app.sh carries
// Sparkle.framework and the update feed in its Info.plist; Sparkle is loaded at run time, so the game itself builds
// without it, and an app without it (or a bare binary) simply has no updater. Other systems get no-op stubs.
namespace MacUpdater
{

#ifdef PVZP_MAC_UPDATER
void	Start();		// on the main thread once the window is up: Sparkle then checks for updates in the background
bool	Available();	// Sparkle is running, so CheckNow() does something
void	CheckNow();		// shows Sparkle's window, which offers the update if there is one
#else
inline void	Start() {}
inline bool	Available() { return false; }
inline void	CheckNow() {}
#endif

}

#endif
