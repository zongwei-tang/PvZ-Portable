/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#include "PhoneLayout.h"
#include <algorithm>
#include <cmath>

namespace PhoneLayout
{

namespace
{

Tile MakeTile(const Box& theSource, double theX, double theY, double theScale, Kind theKind = Kind::Screen)
{
	return Tile{ theSource, Box{ theX, theY, theSource.mWidth * theScale, theSource.mHeight * theScale }, theKind };
}

}

std::vector<Tile> Compute(const Input& theInput)
{
	std::vector<Tile> aTiles;
	const Box& aSafe = theInput.mSafeArea;
	const Box& aLawn = theInput.mLawn;
	if (theInput.mScreenHeight <= 0 || theInput.mScreenWidth / theInput.mScreenHeight < MIN_ASPECT || aSafe.Empty() || aLawn.Empty())
		return aTiles;

	// The game's screen as tall as the window, against the right edge of the safe area, unless that leaves too little
	// room for the cards; then smaller, and centred in the height.
	double aScale = theInput.mScreenHeight / aLawn.mHeight;
	if (aLawn.mWidth * aScale > aSafe.mWidth - MIN_COLUMN_WIDTH - GAP)
		aScale = (aSafe.mWidth - MIN_COLUMN_WIDTH - GAP) / aLawn.mWidth;
	if (aScale <= 0)
		return aTiles;
	double aLawnX = aSafe.Right() - aLawn.mWidth * aScale;
	double aLawnY = (theInput.mScreenHeight - aLawn.mHeight * aScale) / 2;
	auto InPlace = [&](const Box& theSource)
	{
		return MakeTile(theSource, aLawnX + (theSource.mX - aLawn.mX) * aScale, aLawnY + (theSource.mY - aLawn.mY) * aScale, aScale);
	};

	// The backdrop left and right of the game's screen, in line with it, over as much of the window as it covers. The
	// image ends a little right of the safe area's left edge: its part left of the game's screen (the house) is stretched
	// across to it, so that no black shows there.
	if (!theInput.mBackdrop.Empty())
	{
		const Box& aBack = theInput.mBackdrop;
		double aLeft = std::max(aBack.mX, aLawn.mX - aLawnX / aScale);
		double aTop = std::max(aBack.mY, aLawn.mY - aLawnY / aScale);
		double aRight = std::min(aBack.Right(), aLawn.mX + (theInput.mScreenWidth - aLawnX) / aScale);
		double aBottom = std::min(aBack.Bottom(), aLawn.mY + (theInput.mScreenHeight - aLawnY) / aScale);
		if (aRight > aLeft && aBottom > aTop)
		{
			if (aLeft < aLawn.mX)
			{
				Tile aTile = InPlace(Box{ aLeft, aTop, aLawn.mX - aLeft, aBottom - aTop });
				aTile.mKind = Kind::Backdrop;
				double aStretchTo = std::max(0.0, aSafe.mX);
				if (aTile.mDest.mX > aStretchTo)
				{
					aTile.mDest.mX = aStretchTo;
					aTile.mDest.mWidth = aLawnX - aStretchTo;
				}
				aTiles.push_back(aTile);
			}
			if (aRight > aLawn.Right())
			{
				Tile aTile = InPlace(Box{ aLawn.Right(), aTop, aRight - aLawn.Right(), aBottom - aTop });
				aTile.mKind = Kind::Backdrop;
				aTiles.push_back(aTile);
			}
		}
	}

	// The game's screen, which the game draws without the seed bank and what the layout puts elsewhere; over the
	// backdrop the darkness the game draws over its screen, if any
	size_t aNumBackdrops = aTiles.size();
	aTiles.push_back(InPlace(aLawn));
	if (theInput.mDarkWeather)
		for (size_t i = 0; i < aNumBackdrops; i++)
			aTiles.push_back(Tile{ aTiles[i].mSource, aTiles[i].mDest, Kind::Weather });

	// The scale of the cards: as large as the phone versions have them, and smaller when they would not fit down the
	// window (past the home indicator, as the phone versions have them) or left of the game's screen. On a conveyor belt
	// the cards ride up a belt as long as the window, with room for CONVEYOR_SLOTS of them.
	size_t n = theInput.mPackets.size();
	bool aConveyor = !theInput.mConveyorBelt.Empty() && theInput.mPacketSlots.size() == n;
	double aSlots = aConveyor ? CONVEYOR_SLOTS : static_cast<double>(n);
	double aPad = aConveyor ? CONVEYOR_PAD : 0;
	double aCardScale = MAX_CARD_HEIGHT * theInput.mScreenHeight / CARD_HEIGHT;
	if (aSlots > 0)
		aCardScale = std::min(aCardScale, (theInput.mScreenHeight - aSafe.mY - 2 * EDGE_MARGIN - (aSlots - 1) * CARD_GAP) / (aSlots * CARD_HEIGHT));
	aCardScale = std::min(aCardScale, (aLawnX - GAP - aSafe.mX - EDGE_MARGIN) / (CARD_WIDTH + 2 * aPad));
	if (aCardScale <= 0)
		return {};
	double aColumnRight = aSafe.mX + EDGE_MARGIN + (n > 0 || aConveyor ? (CARD_WIDTH + 2 * aPad) * aCardScale : 0);

	// Along the top, right of the cards: the sun counter, then the shovel, all as high as the sun counter
	double aBarX = aColumnRight + GAP;
	double aBarY = aSafe.mY + EDGE_MARGIN;
	double aBarHeight = SUN_COUNTER_HEIGHT * aCardScale;
	Tile aSunCounter;
	bool aHasSun = !theInput.mSunCounter.Empty();
	if (aHasSun)
	{
		aSunCounter = Tile{ theInput.mSunCounter, Box{ aBarX, aBarY, SUN_COUNTER_WIDTH * aCardScale, aBarHeight }, Kind::SunCounter };
		aBarX += aSunCounter.mDest.mWidth + GAP;
	}
	if (!theInput.mShovel.Empty())
	{
		Tile aShovel = MakeTile(theInput.mShovel, aBarX, aBarY, aBarHeight / theInput.mShovel.mHeight, Kind::Shovel);
		aTiles.push_back(aShovel);
		aBarX += aShovel.mDest.mWidth + GAP;
	}

	// The menu button in the top right corner, larger than in the game's screen, as high as the sun counter and the
	// margin above it (MIN_MENU_HEIGHT at the least, to be easy to tap), over where the game's screen has it
	double aMenuLeft = aSafe.Right() - EDGE_MARGIN, aMenuTop = aBarY, aMenuHeight = 0;
	if (!theInput.mMenuButton.Empty())
	{
		Tile aInPlace = InPlace(theInput.mMenuButton);
		double aRightEdge = std::min(aInPlace.mDest.Right(), aSafe.Right());
		double aMenuScale = std::max({ aInPlace.mDest.mHeight, aBarY - aSafe.mY + aBarHeight, MIN_MENU_HEIGHT }) / theInput.mMenuButton.mHeight;
		Tile aMenu = MakeTile(theInput.mMenuButton, aRightEdge - theInput.mMenuButton.mWidth * aMenuScale, aSafe.mY, aMenuScale, Kind::Menu);
		aTiles.push_back(aMenu);
		aMenuLeft = aMenu.mDest.mX;
		aMenuTop = aMenu.mDest.mY;
		aMenuHeight = aMenu.mDest.mHeight;
	}

	// The progress meter left of the menu button, as high as the button (or the sun counter) and centred on it; smaller when
	// that leaves too little room right of the shovel, and not shown when it would be too small to read
	if (!theInput.mMeter.Empty())
	{
		double aMeterScale = std::max(aBarHeight, aMenuHeight) / theInput.mMeter.mHeight;
		double aRoom = aMenuLeft - GAP - aBarX;
		double aPlanned = aMeterScale;
		if (theInput.mMeter.mWidth * aMeterScale > aRoom)
			aMeterScale = aRoom / theInput.mMeter.mWidth;
		if (aMeterScale > aPlanned * 0.5)
		{
			double aHeight = theInput.mMeter.mHeight * aMeterScale;
			double aY = aMenuHeight > 0 ? aMenuTop + (aMenuHeight - aHeight) / 2 : aBarY;
			aTiles.push_back(MakeTile(theInput.mMeter, aMenuLeft - GAP - theInput.mMeter.mWidth * aMeterScale, std::max(aBarY, aY), aMeterScale, Kind::Meter));
		}
	}
	if (aHasSun)
		aTiles.push_back(aSunCounter);

	// The cards, one column down the left edge of the safe area from its top; on a conveyor belt each where its packet
	// is along the belt, over the belt, and only those on the window
	double aCardX = aSafe.mX + EDGE_MARGIN + aPad * aCardScale;
	double aTop = aSafe.mY + EDGE_MARGIN;
	double aPitch = CARD_HEIGHT * aCardScale + CARD_GAP;
	if (aConveyor)
		aTiles.push_back(Tile{ theInput.mConveyorBelt, Box{ aSafe.mX + EDGE_MARGIN, aSafe.mY, (CARD_WIDTH + 2 * aPad) * aCardScale, theInput.mScreenHeight - aSafe.mY },
			Kind::Conveyor });
	for (size_t i = 0; i < n; i++)
	{
		double aY = aTop + (aConveyor ? std::max(0.0, theInput.mPacketSlots[i]) : static_cast<double>(i)) * aPitch;
		if (aY >= theInput.mScreenHeight)
			continue;
		aTiles.push_back(Tile{ theInput.mPackets[i], Box{ aCardX, aY, CARD_WIDTH * aCardScale, CARD_HEIGHT * aCardScale }, Kind::Card,
			static_cast<int>(i) });
	}
	return aTiles;
}

std::vector<Tile> ComputeFramed(double theScreenWidth, double theScreenHeight, const Box& theSafeArea, const Box& theScreen)
{
	std::vector<Tile> aTiles;
	if (theScreenHeight <= 0 || theScreenWidth / theScreenHeight < MIN_ASPECT || theSafeArea.Empty() || theScreen.Empty())
		return aTiles;
	// the blurred copy: the middle of the game's screen, as wide as it and as high as the window's shape gives
	double aHeight = std::min(theScreen.mHeight, theScreen.mWidth * theScreenHeight / theScreenWidth);
	aTiles.push_back(Tile{ Box{ theScreen.mX, theScreen.mY + (theScreen.mHeight - aHeight) / 2, theScreen.mWidth, aHeight },
		Box{ 0, 0, theScreenWidth, theScreenHeight }, Kind::Blur });
	double aScale = std::min(theScreenHeight / theScreen.mHeight, theSafeArea.mWidth / theScreen.mWidth);
	aTiles.push_back(MakeTile(theScreen, theSafeArea.mX + (theSafeArea.mWidth - theScreen.mWidth * aScale) / 2,
		(theScreenHeight - theScreen.mHeight * aScale) / 2, aScale));
	return aTiles;
}

bool WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY)
{
	for (auto anItr = theTiles.rbegin(); anItr != theTiles.rend(); ++anItr)
	{
		const Tile& aTile = *anItr;
		if (!aTile.TakesTaps() || !aTile.mDest.Contains(theX, theY))
			continue;
		if (aTile.TakesNoTap())
			return false;
		theGameX = aTile.mSource.mX + (theX - aTile.mDest.mX) * aTile.mSource.mWidth / aTile.mDest.mWidth;
		theGameY = aTile.mSource.mY + (theY - aTile.mDest.mY) * aTile.mSource.mHeight / aTile.mDest.mHeight;
		return true;
	}
	return false;
}

}
