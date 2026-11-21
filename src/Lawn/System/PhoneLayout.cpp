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

void PacketSize(const Input& theInput, double& theWidth, double& theHeight)
{
	theWidth = theHeight = 0;
	for (const Box& aPacket : theInput.mPackets)
	{
		theWidth = std::max(theWidth, aPacket.mWidth);
		theHeight = std::max(theHeight, aPacket.mHeight);
	}
}

// The size, in the game's units, of the packets laid out theColumns side by side
void PacketsSize(const Input& theInput, int theColumns, double& theWidth, double& theHeight)
{
	double aPacketWidth = 0, aPacketHeight = 0;
	PacketSize(theInput, aPacketWidth, aPacketHeight);
	int aRows = theInput.mPackets.empty() ? 0 : (static_cast<int>(theInput.mPackets.size()) + theColumns - 1) / theColumns;
	theWidth = aRows > 0 ? theColumns * aPacketWidth + (theColumns - 1) * ITEM_GAP : 0;
	theHeight = aRows > 0 ? aRows * aPacketHeight + (aRows - 1) * ITEM_GAP : 0;
}

bool Within(const Box& theInner, const Box& theOuter)
{
	return theInner.mX >= theOuter.mX && theInner.mY >= theOuter.mY && theInner.Right() <= theOuter.Right() && theInner.Bottom() <= theOuter.Bottom();
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

	// The game's screen as tall as the window, against the right edge of the safe area, unless that leaves too little
	// room for the packets; then smaller, and centred in the height.
	double aScale = theInput.mScreenHeight / aLawn.mHeight;
	if (aLawn.mWidth * aScale > aSafe.mWidth - MIN_COLUMN_WIDTH - COLUMN_GAP)
		aScale = (aSafe.mWidth - MIN_COLUMN_WIDTH - COLUMN_GAP) / aLawn.mWidth;
	if (aScale <= 0)
		return aTiles;
	// across: the house of the backdrop from the left edge of the safe area, so that only the notch side stays dark,
	// unless that takes the game's screen past the right edge of the safe area (or there is no backdrop); then against it
	double aLawnX = aSafe.Right() - aLawn.mWidth * aScale;
	if (!theInput.mBackdrop.Empty() && theInput.mBackdrop.mX < aLawn.mX)
		aLawnX = std::min(aLawnX, aSafe.mX + (aLawn.mX - theInput.mBackdrop.mX) * aScale);
	double aLawnY = (theInput.mScreenHeight - aLawn.mHeight * aScale) / 2;
	auto InPlace = [&](const Box& theSource)
	{
		return MakeTile(theSource, aLawnX + (theSource.mX - aLawn.mX) * aScale, aLawnY + (theSource.mY - aLawn.mY) * aScale, aScale);
	};

	// The backdrop behind it all, in line with the game's screen, over as much of the window as it covers
	if (!theInput.mBackdrop.Empty())
	{
		const Box& aBack = theInput.mBackdrop;
		double aLeft = std::max(aBack.mX, aLawn.mX - aLawnX / aScale);
		double aTop = std::max(aBack.mY, aLawn.mY - aLawnY / aScale);
		double aRight = std::min(aBack.Right(), aLawn.mX + (theInput.mScreenWidth - aLawnX) / aScale);
		double aBottom = std::min(aBack.Bottom(), aLawn.mY + (theInput.mScreenHeight - aLawnY) / aScale);
		if (aRight > aLeft && aBottom > aTop)
		{
			Tile aTile = InPlace(Box{ aLeft, aTop, aRight - aLeft, aBottom - aTop });
			aTile.mBackdrop = true;
			aTiles.push_back(aTile);
		}
	}

	// The game's screen without the seed bank, in two pieces: right of mBankRight all of it (the top strip there holds
	// the menu button), left of it only below the bank. Where the bank was, the backdrop shows.
	double aBankBottom = std::clamp(theInput.mBankBottom, aLawn.mY, aLawn.Bottom());
	Box aRight{ theInput.mBankRight, aLawn.mY, aLawn.Right() - theInput.mBankRight, aLawn.mHeight };
	Box aLeft{ aLawn.mX, aBankBottom, theInput.mBankRight - aLawn.mX, aLawn.Bottom() - aBankBottom };
	aTiles.push_back(InPlace(aRight));
	if (!aLeft.Empty())
		aTiles.push_back(InPlace(aLeft));

	// Of the strip along the top, the sun counter and the shovel stay where they are, and so does the menu button
	if (!theInput.mSunCounter.Empty())
		aTiles.push_back(InPlace(theInput.mSunCounter));
	if (!theInput.mShovel.Empty())
		aTiles.push_back(InPlace(theInput.mShovel));
	if (!theInput.mMenuButton.Empty() && !Within(theInput.mMenuButton, aRight))
		aTiles.push_back(InPlace(theInput.mMenuButton));

	// The packets in a column over the house, left of the game's screen, as large as fit with one to three side by side
	if (!theInput.mPackets.empty())
	{
		Box aColumn{ aSafe.mX + EDGE_MARGIN, aSafe.mY + EDGE_MARGIN, aLawnX - COLUMN_GAP - aSafe.mX - EDGE_MARGIN,
			aSafe.mHeight - 2 * EDGE_MARGIN };
		if (aColumn.mWidth <= 0 || aColumn.mHeight <= 0)
			return {};
		int aColumns = 1;
		double aColumnScale = 0;
		for (int aTry = 1; aTry <= 3; aTry++)
		{
			double aWidth = 0, aHeight = 0;
			PacketsSize(theInput, aTry, aWidth, aHeight);
			double aFit = std::min({ aColumn.mWidth / aWidth, aColumn.mHeight / aHeight, MAX_COLUMN_SCALE });
			if (aFit > aColumnScale + 1e-9)
			{
				aColumnScale = aFit;
				aColumns = aTry;
			}
		}
		if (aColumnScale <= 0)
			return {};

		double aWidth = 0, aHeight = 0, aPacketWidth = 0, aPacketHeight = 0;
		PacketsSize(theInput, aColumns, aWidth, aHeight);
		PacketSize(theInput, aPacketWidth, aPacketHeight);
		double aX = aColumn.mX + (aColumn.mWidth - aWidth * aColumnScale) / 2;
		for (size_t i = 0; i < theInput.mPackets.size(); i++)
		{
			int aRow = static_cast<int>(i) / aColumns;
			int aCol = static_cast<int>(i) % aColumns;
			aTiles.push_back(MakeTile(theInput.mPackets[i], aX + aCol * (aPacketWidth + ITEM_GAP) * aColumnScale,
				aColumn.mY + aRow * (aPacketHeight + ITEM_GAP) * aColumnScale, aColumnScale));
		}
	}
	return aTiles;
}

bool WindowToGame(const std::vector<Tile>& theTiles, double theX, double theY, double& theGameX, double& theGameY)
{
	for (auto anItr = theTiles.rbegin(); anItr != theTiles.rend(); ++anItr)
	{
		const Tile& aTile = *anItr;
		if (aTile.mBackdrop || !aTile.mDest.Contains(theX, theY))
			continue;
		theGameX = aTile.mSource.mX + (theX - aTile.mDest.mX) * aTile.mSource.mWidth / aTile.mDest.mWidth;
		theGameY = aTile.mSource.mY + (theY - aTile.mDest.mY) * aTile.mSource.mHeight / aTile.mDest.mHeight;
		return true;
	}
	return false;
}

}
