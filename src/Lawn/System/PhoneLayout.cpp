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

Tile MakeTile(const Box& theSource, double theX, double theY, double theScale)
{
	return Tile{ theSource, Box{ theX, theY, theSource.mWidth * theScale, theSource.mHeight * theScale } };
}

// The size, in the game's units, of the column laid out with theColumns packets side by side: the menu button on top,
// then the sun counter with the shovel beside it, then the packets.
void ColumnSize(const Input& theInput, int theColumns, double& theWidth, double& theHeight)
{
	double aPacketWidth = 0, aPacketHeight = 0;
	for (const Box& aPacket : theInput.mPackets)
	{
		aPacketWidth = std::max(aPacketWidth, aPacket.mWidth);
		aPacketHeight = std::max(aPacketHeight, aPacket.mHeight);
	}
	int aRows = theInput.mPackets.empty() ? 0 : (static_cast<int>(theInput.mPackets.size()) + theColumns - 1) / theColumns;

	double aSunRowWidth = theInput.mSunCounter.mWidth + (theInput.mShovel.Empty() ? 0 : ITEM_GAP + theInput.mShovel.mWidth);
	double aSunRowHeight = std::max(theInput.mSunCounter.mHeight, theInput.mShovel.Empty() ? 0.0 : theInput.mShovel.mHeight);
	double aPacketsWidth = aRows > 0 ? theColumns * aPacketWidth + (theColumns - 1) * ITEM_GAP : 0;
	double aPacketsHeight = aRows > 0 ? aRows * aPacketHeight + (aRows - 1) * ITEM_GAP : 0;

	theWidth = std::max({ theInput.mMenuButton.Empty() ? 0.0 : theInput.mMenuButton.mWidth, aSunRowWidth, aPacketsWidth });
	theHeight = (theInput.mMenuButton.Empty() ? 0 : theInput.mMenuButton.mHeight + ITEM_GAP) + aSunRowHeight +
		(aRows > 0 ? ITEM_GAP + aPacketsHeight : 0);
}

}

std::vector<Tile> Compute(const Input& theInput)
{
	std::vector<Tile> aTiles;
	const Box& aSafe = theInput.mSafeArea;
	const Box& aLawn = theInput.mLawn;
	if (theInput.mScreenHeight <= 0 || theInput.mScreenWidth / theInput.mScreenHeight < MIN_ASPECT || aSafe.Empty() || aLawn.Empty())
		return aTiles;
	if (theInput.mBankRight <= aLawn.mX || theInput.mBankRight >= aLawn.Right())
		return aTiles;

	// The lawn as tall as the screen, against the right edge of the safe area, unless that leaves too little room for
	// the column; then smaller, and centred in the height.
	double aScale = theInput.mScreenHeight / aLawn.mHeight;
	if (aLawn.mWidth * aScale > aSafe.mWidth - MIN_COLUMN_WIDTH - COLUMN_GAP)
		aScale = (aSafe.mWidth - MIN_COLUMN_WIDTH - COLUMN_GAP) / aLawn.mWidth;
	if (aScale <= 0)
		return aTiles;
	double aLawnX = aSafe.Right() - aLawn.mWidth * aScale;
	double aLawnY = (theInput.mScreenHeight - aLawn.mHeight * aScale) / 2;

	// Two pieces: left of mBankRight only below the bank (above it the bank was drawn), right of it all of the lawn, so
	// that the heads of the zombies in the top row stay in sight. The corner the bank took stays empty.
	double aBankBottom = std::clamp(theInput.mBankBottom, aLawn.mY, aLawn.Bottom());
	Box aLeft{ aLawn.mX, aBankBottom, theInput.mBankRight - aLawn.mX, aLawn.Bottom() - aBankBottom };
	Box aRight{ theInput.mBankRight, aLawn.mY, aLawn.Right() - theInput.mBankRight, aLawn.mHeight };
	aTiles.push_back(MakeTile(aRight, aLawnX + (aRight.mX - aLawn.mX) * aScale, aLawnY, aScale));
	if (!aLeft.Empty())
		aTiles.push_back(MakeTile(aLeft, aLawnX, aLawnY + (aBankBottom - aLawn.mY) * aScale, aScale));

	// The column between the left edge of the safe area and the lawn, with as many packets side by side as make them
	// largest
	Box aColumn{ aSafe.mX, aSafe.mY, aLawnX - COLUMN_GAP - aSafe.mX, aSafe.mHeight };
	if (aColumn.mWidth <= 0)
		return {};
	int aColumns = 1;
	double aColumnScale = 0;
	for (int aTry = 1; aTry <= 3; aTry++)
	{
		double aWidth = 0, aHeight = 0;
		ColumnSize(theInput, aTry, aWidth, aHeight);
		if (aWidth <= 0 || aHeight <= 0)
			continue;
		double aFit = std::min({ aColumn.mWidth / aWidth, aColumn.mHeight / aHeight, MAX_COLUMN_SCALE });
		if (aFit > aColumnScale + 1e-9)
		{
			aColumnScale = aFit;
			aColumns = aTry;
		}
	}
	if (aColumnScale <= 0)
		return {};

	double aWidth = 0, aHeight = 0;
	ColumnSize(theInput, aColumns, aWidth, aHeight);
	double aLeftX = aColumn.mX + (aColumn.mWidth - aWidth * aColumnScale) / 2;
	double aY = aColumn.mY + (aColumn.mHeight - aHeight * aColumnScale) / 2;
	double aCentreX = aLeftX + aWidth * aColumnScale / 2;
	double aGap = ITEM_GAP * aColumnScale;

	if (!theInput.mMenuButton.Empty())
	{
		aTiles.push_back(MakeTile(theInput.mMenuButton, aCentreX - theInput.mMenuButton.mWidth * aColumnScale / 2, aY, aColumnScale));
		aY += theInput.mMenuButton.mHeight * aColumnScale + aGap;
	}

	double aSunRowWidth = theInput.mSunCounter.mWidth + (theInput.mShovel.Empty() ? 0 : ITEM_GAP + theInput.mShovel.mWidth);
	double aSunRowHeight = std::max(theInput.mSunCounter.mHeight, theInput.mShovel.Empty() ? 0.0 : theInput.mShovel.mHeight);
	double aX = aCentreX - aSunRowWidth * aColumnScale / 2;
	if (!theInput.mSunCounter.Empty())
		aTiles.push_back(MakeTile(theInput.mSunCounter, aX, aY, aColumnScale));
	if (!theInput.mShovel.Empty())
		aTiles.push_back(MakeTile(theInput.mShovel, aX + (theInput.mSunCounter.mWidth + ITEM_GAP) * aColumnScale, aY, aColumnScale));
	aY += aSunRowHeight * aColumnScale + aGap;

	double aPacketWidth = 0, aPacketHeight = 0;
	for (const Box& aPacket : theInput.mPackets)
	{
		aPacketWidth = std::max(aPacketWidth, aPacket.mWidth);
		aPacketHeight = std::max(aPacketHeight, aPacket.mHeight);
	}
	double aRowWidth = aColumns * aPacketWidth + (aColumns - 1) * ITEM_GAP;
	for (size_t i = 0; i < theInput.mPackets.size(); i++)
	{
		int aRow = static_cast<int>(i) / aColumns;
		int aCol = static_cast<int>(i) % aColumns;
		double aPacketX = aCentreX - aRowWidth * aColumnScale / 2 + aCol * (aPacketWidth + ITEM_GAP) * aColumnScale;
		double aPacketY = aY + aRow * (aPacketHeight + ITEM_GAP) * aColumnScale;
		aTiles.push_back(MakeTile(theInput.mPackets[i], aPacketX, aPacketY, aColumnScale));
	}
	return aTiles;
}

bool WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY)
{
	for (const Tile& aTile : theTiles)
	{
		if (!aTile.mDest.Contains(theX, theY))
			continue;
		theGameX = aTile.mSource.mX + (theX - aTile.mDest.mX) * aTile.mSource.mWidth / aTile.mDest.mWidth;
		theGameY = aTile.mSource.mY + (theY - aTile.mDest.mY) * aTile.mSource.mHeight / aTile.mDest.mHeight;
		return true;
	}
	return false;
}

}
