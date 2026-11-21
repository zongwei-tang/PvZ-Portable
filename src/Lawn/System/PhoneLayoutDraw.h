/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifndef __PHONELAYOUTDRAW_H__
#define __PHONELAYOUTDRAW_H__

class LawnApp;
namespace Sexy { struct PresentationTile; }

// What the phone layout (PhoneLayout.h) draws into the window itself, over the pieces of the game's screen: the cards of
// the packets, the sun counter, the shovel, the progress meter and the menu button, made of the game's own images and
// drawn with its own functions.
namespace PhoneLayoutDraw
{

// the progress meter, in the game's screen: where Board::DrawProgressMeter draws it
constexpr double	METER_X = 580, METER_Y = 555, METER_WIDTH = 180, METER_HEIGHT = 47;

// Draws an overlay tile (GLInterface::mDrawOverlay), in window points
void	Overlay(LawnApp* theApp, const Sexy::PresentationTile& theTile);

}

#endif
