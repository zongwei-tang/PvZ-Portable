/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSAVE_H__
#define __CLOUDSAVE_H__

// Keeps userdata/ in sync with a WebDAV folder configured in cloudsave.ini (see CloudSync.h for the protocol).
// Builds without libcurl (PVZP_CLOUD_SAVE undefined) get no-op stubs.
namespace CloudSave
{

#ifdef PVZP_CLOUD_SAVE
void	Init();						// before the profiles are loaded: pulls a newer cloud save, or notes a conflict
void	Update();					// every frame: uploads changed saves in the background, asks about conflicts on the main menu
void	Shutdown();					// after the last saves were written: uploads them before the game exits
void	ResolveConflict(bool theUseCloud);
#else
inline void	Init() {}
inline void	Update() {}
inline void	Shutdown() {}
inline void	ResolveConflict(bool) {}
#endif

}

#endif
