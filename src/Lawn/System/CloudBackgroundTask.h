/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDBACKGROUNDTASK_H__
#define __CLOUDBACKGROUNDTASK_H__

// More time to finish an upload once iOS sent the game to the background (UIApplication beginBackgroundTask): without
// it iOS stops the game within seconds, in the middle of the upload. Elsewhere a program keeps running in the
// background, and these do nothing.
namespace CloudBackgroundTask
{

#if defined(PVZP_CLOUD_SAVE) && defined(PVZP_CLOUD_SIGN_IN_SHEET)
void	Begin();	// on the main thread; one at a time, a second Begin() keeps the first
void	End();		// from any thread; nothing if none was begun
#else
inline void	Begin() {}
inline void	End() {}
#endif

}

#endif
