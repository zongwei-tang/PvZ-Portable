/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifndef __PHONELAYOUTSELFTEST_H__
#define __PHONELAYOUTSELFTEST_H__

#ifdef PVZP_PHONE_LAYOUT_SELFTEST

#include <SDL3/SDL.h>

class LawnApp;

// A test of the phone layout in the running game, built with -DPHONE_LAYOUT_SELFTEST=ON and run by
// tests/phone_layout_selftest.sh: it sizes the window as a phone's, plays some levels by itself, taps packets, the lawn,
// the shovel and the menu button through the tiles, reads back what is drawn and checks it, and writes what it found as
// text. Nothing of it is in other builds.
namespace PhoneLayoutSelfTest
{

void	Update(LawnApp* theApp);			// once per LawnApp::UpdateFrames
void	SafeArea(SDL_Rect& theSafeArea);	// the safe area of the phone tested, in place of the window's
bool	SuppressTiles();					// true for a frame drawn letterboxed, to compare with

}

#endif

#endif
