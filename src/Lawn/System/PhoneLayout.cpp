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

bool Within(const Box& theInner, const Box& theOuter)
{
	return theInner.mX >= theOuter.mX && theInner.mY >= theOuter.mY && theInner.Right() <= theOuter.Right() && theInner.Bottom() <= theOuter.Bottom();
}

// The two pieces a packet is shown in, side by side as a card: its picture, and its cost from the strip below it
void PacketPieces(const Box& thePacket, Box& thePicture, Box& theCost)
{
	thePicture = Box{ thePacket.mX, thePacket.mY, thePacket.mWidth, PACKET_PICTURE_HEIGHT };
	theCost = Box{ thePacket.mX + PACKET_COST_X, thePacket.mY + PACKET_PICTURE_HEIGHT, PACKET_COST_WIDTH, thePacket.mHeight - PACKET_PICTURE_HEIGHT };
}

// The two pieces of the sun counter, side by side: the sun, and the number below it
void SunPieces(const Box& theCounter, Box& theSun, Box& theNumber)
{
	double aSunHeight = std::min(SUN_PICTURE_HEIGHT, theCounter.mHeight);
	theSun = Box{ theCounter.mX, theCounter.mY, theCounter.mWidth, aSunHeight };
	theNumber = Box{ theCounter.mX, theCounter.mY + aSunHeight, theCounter.mWidth, theCounter.mHeight - aSunHeight };
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
	double aLawnX = aSafe.Right() - aLawn.mWidth * aScale;
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
	if (!theInput.mMenuButton.Empty() && !Within(theInput.mMenuButton, aRight))
		aTiles.push_back(InPlace(theInput.mMenuButton));

	// The packets as cards, picture and cost side by side, in one column down the left edge of the safe area
	double aColumnRight = aSafe.mX + EDGE_MARGIN;
	if (!theInput.mPackets.empty())
	{
		double aCardWidth = 0, aCardHeight = 0;
		for (const Box& aPacket : theInput.mPackets)
		{
			Box aPicture, aCost;
			PacketPieces(aPacket, aPicture, aCost);
			aCardWidth = std::max(aCardWidth, aPicture.mWidth + aCost.mWidth);
			aCardHeight = std::max(aCardHeight, aPicture.mHeight);
		}
		size_t n = theInput.mPackets.size();
		// down to the bottom of the screen, past the home indicator, as the phone versions have them
		double aCardScale = std::min({ MAX_CARD_SCALE, (theInput.mScreenHeight - aSafe.mY - 2 * EDGE_MARGIN) / (n * aCardHeight + (n - 1) * CARD_GAP),
			(aLawnX - COLUMN_GAP - aSafe.mX - EDGE_MARGIN) / aCardWidth });
		if (aCardScale <= 0)
			return {};
		double aY = aSafe.mY + EDGE_MARGIN;
		for (const Box& aPacket : theInput.mPackets)
		{
			Box aPicture, aCost;
			PacketPieces(aPacket, aPicture, aCost);
			double aX = aSafe.mX + EDGE_MARGIN;
			aTiles.push_back(MakeTile(aPicture, aX, aY, aCardScale));
			aTiles.push_back(MakeTile(aCost, aX + aPicture.mWidth * aCardScale, aY + (aPicture.mHeight - aCost.mHeight) * aCardScale, aCardScale));
			aY += (aCardHeight + CARD_GAP) * aCardScale;
		}
		aColumnRight = aSafe.mX + EDGE_MARGIN + aCardWidth * aCardScale;
	}

	// Along the top, right of the cards: the sun counter, its sun and number side by side, then the shovel
	double aBarX = aColumnRight + COLUMN_GAP;
	double aBarY = aSafe.mY + EDGE_MARGIN;
	if (!theInput.mSunCounter.Empty())
	{
		Box aSun, aNumber;
		SunPieces(theInput.mSunCounter, aSun, aNumber);
		double aBarScale = BAR_HEIGHT / aSun.mHeight;
		aTiles.push_back(MakeTile(aSun, aBarX, aBarY, aBarScale));
		aBarX += aSun.mWidth * aBarScale;
		if (!aNumber.Empty())
		{
			aTiles.push_back(MakeTile(aNumber, aBarX, aBarY + (aSun.mHeight - aNumber.mHeight) / 2 * aBarScale, aBarScale));
			aBarX += aNumber.mWidth * aBarScale;
		}
		aBarX += COLUMN_GAP;
	}
	if (!theInput.mShovel.Empty())
		aTiles.push_back(MakeTile(theInput.mShovel, aBarX, aBarY, BAR_HEIGHT / theInput.mShovel.mHeight));
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
