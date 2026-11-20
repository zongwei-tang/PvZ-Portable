/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifndef __PHONELAYOUT_H__
#define __PHONELAYOUT_H__

#include <vector>

// The layout of a level on a wide phone screen. The game still draws its 800x600 screen as everywhere else; this cuts it
// into tiles and places them on the phone: the lawn as tall as the screen, and in a column at the side the menu button,
// the sun counter, the shovel and the seed packets, which on the 800x600 screen lie in the strip along the top. A tap is
// mapped back through the tile under it, so every part of the game handles it as before.
//
// Only geometry, without the game, GL or SDL, so that tests can check it (tests/phone_layout_test.cpp).
namespace PhoneLayout
{

struct Box
{
	double	mX = 0;
	double	mY = 0;
	double	mWidth = 0;
	double	mHeight = 0;

	double	Right() const { return mX + mWidth; }
	double	Bottom() const { return mY + mHeight; }
	bool	Empty() const { return mWidth <= 0 || mHeight <= 0; }
	bool	Contains(double theX, double theY) const { return theX >= mX && theX < Right() && theY >= mY && theY < Bottom(); }
};

// A piece of the game's screen (mSource, in its 800x600 coordinates) and where it goes in the window (mDest, in window
// points). Source and destination have the same aspect, so a tile is scaled alike in both directions.
struct Tile
{
	Box		mSource;
	Box		mDest;
};

struct Input
{
	double				mScreenWidth = 0;	// the window, in points
	double				mScreenHeight = 0;
	Box					mSafeArea;			// the part of the window clear of the notch or Dynamic Island, in points

	Box					mLawn;				// the part of the game's screen to show as the lawn
	double				mBankRight = 0;		// right of this the top strip holds nothing of the bank or the shovel
	double				mBankBottom = 0;	// below this the lawn is clear of the bank

	Box					mMenuButton;		// in the game's screen; empty when it is not shown
	Box					mSunCounter;
	Box					mShovel;			// empty without a shovel
	std::vector<Box>	mPackets;
};

constexpr double	MIN_ASPECT = 1.65;			// narrower screens (iPad, 4:3 and 16:10) keep the 800x600 screen
constexpr double	MIN_COLUMN_WIDTH = 96;		// points left of the lawn for the column, at the least
constexpr double	COLUMN_GAP = 6;				// points between the column and the lawn
constexpr double	ITEM_GAP = 5;				// between the items of the column, in the game's units
constexpr double	MAX_COLUMN_SCALE = 1.6;		// points per unit of the game's screen, at the most, in the column

// The tiles, the lawn first. Empty when the screen is not wide enough for the layout to gain anything; the game then
// shows its 800x600 screen as usual.
std::vector<Tile>	Compute(const Input& theInput);

// The point of the game's screen under a window point; false when it is under no tile.
bool				WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY);

}

#endif
