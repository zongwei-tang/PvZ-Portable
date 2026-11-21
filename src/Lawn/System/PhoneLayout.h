/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifndef __PHONELAYOUT_H__
#define __PHONELAYOUT_H__

#include <vector>

// The layout of a level on a wide phone screen, after the phone versions of the game: the game's screen as tall as the
// window and against its right side, with the house of the level's backdrop left of it; the seed packets as cards (the
// picture, and the cost beside it) in a column down the left edge; along the top, right of the cards, the sun counter
// (its sun and its number side by side) and the shovel; the menu button where it is, in the top right corner. Where the
// seed bank was, the backdrop shows. The game still draws its 800x600 screen as everywhere else; this cuts it into tiles
// and places them on the phone over the backdrop. A tap is mapped back through the topmost tile under it (not the
// backdrop), so every part of the game handles it as before.
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
	bool	mBackdrop = false;	// a piece of the backdrop image (mSource in the game's coordinates all the same), which no tap goes to
};

struct Input
{
	double				mScreenWidth = 0;	// the window, in points
	double				mScreenHeight = 0;
	Box					mSafeArea;			// the part of the window clear of the notch or Dynamic Island, in points

	Box					mLawn;				// the part of the game's screen to show, the lawn and the strip above it
	Box					mBackdrop;			// where the backdrop image of the level lies, in the game's coordinates; empty without
	double				mBankRight = 0;		// right of this the top strip holds nothing of the bank or the shovel
	double				mBankBottom = 0;	// below this the lawn is clear of the bank

	Box					mMenuButton;		// in the game's screen; empty when it is not shown
	Box					mSunCounter;
	Box					mShovel;			// empty without a shovel
	std::vector<Box>	mPackets;
};

constexpr double	MIN_ASPECT = 1.65;			// narrower screens (iPad, 4:3 and 16:10) keep the 800x600 screen
constexpr double	MIN_COLUMN_WIDTH = 96;		// points left of the game's screen for the packets, at the least
constexpr double	COLUMN_GAP = 6;				// points between the cards and what is right of them
constexpr double	CARD_GAP = 3;				// between the cards of the packets, in the game's units
constexpr double	MAX_CARD_SCALE = 0.9;		// points per unit of the game's screen, at the most, for the cards of the packets
constexpr double	BAR_HEIGHT = 36;			// points high, the sun counter and the shovel along the top
// the pieces of a packet (its box, with a margin of 2 around the 50x70 packet): its picture above y 52, and its cost below,
// the number of which ends at x 32 of the packet
constexpr double	PACKET_PICTURE_HEIGHT = 52;
constexpr double	PACKET_COST_X = 2;
constexpr double	PACKET_COST_WIDTH = 34;
constexpr double	SUN_PICTURE_HEIGHT = 61;	// of the sun counter, the sun; its number below
constexpr double	EDGE_MARGIN = 4;			// points between the controls and the edges of the safe area

// The tiles, drawn in this order: the backdrop (when there is one), the game's screen in one or two pieces, the menu
// button when those do not hold it, the packets (two tiles each), the sun counter (two tiles), the shovel. Empty when the screen is not wide enough for the layout to gain anything; the
// game then shows its 800x600 screen as usual.
std::vector<Tile>	Compute(const Input& theInput);

// The point of the game's screen under a window point, through the topmost tile there that is not the backdrop; false
// when there is none.
bool				WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY);

}

#endif
