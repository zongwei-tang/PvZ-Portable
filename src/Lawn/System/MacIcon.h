/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#ifndef __MACICON_H__
#define __MACICON_H__

#include <string>

// The icon of the Mac app from the player's own picture: an icon.png in the game data folder (next to main.pak) becomes
// the icon in the Dock and in Finder. The app the releases carry has the icon of the repository, and an update through
// Sparkle replaces the whole app, so the game puts the player's icon back on each start. Other systems get a no-op stub.
namespace MacIcon
{

#ifdef PVZP_MAC_ICON
void	Apply(const std::string& thePicturePath);	// on the main thread once the window is up
#else
inline void	Apply(const std::string&) {}
#endif

}

#endif
