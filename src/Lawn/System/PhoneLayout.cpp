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

// The game's screen as tall as the window, against the right edge of the safe area, unless that leaves too little room
// for the cards; then smaller, and centred in the height. Before it the backdrop left and right of it, in line with it,
// at its scale, over as much of the window as the image covers; past its ends, out to the window's edges, its edge going
// on (Edge). When the board is panned to the house (in a cutscene) the screen further left, so that the house is no
// narrower beside it than in a level; when the image would end short of the window's right edge (the board panned to the
// street, for the seed chooser), the screen further right, out to the window's edge at the most. Without a backdrop what
// goes on around the screen (mSurround), if anything. After it the darkness the game draws over its screen, if any, over
// the backdrop. False when there is no room.
bool PlaceScreen(const Input& theInput, std::vector<Tile>& theTiles, double& theScale, double& theLawnX, double& theLawnY)
{
	const Box& aSafe = theInput.mSafeArea;
	const Box& aLawn = theInput.mLawn;
	double aScale = theInput.mScreenHeight / aLawn.mHeight;
	if (aLawn.mWidth * aScale > aSafe.mWidth - MIN_COLUMN_WIDTH - GAP)
		aScale = (aSafe.mWidth - MIN_COLUMN_WIDTH - GAP) / aLawn.mWidth;
	if (aScale <= 0)
		return false;
	double aLawnX = aSafe.Right() - aLawn.mWidth * aScale;
	double aLawnY = (theInput.mScreenHeight - aLawn.mHeight * aScale) / 2;
	if (!theInput.mBackdrop.Empty())
	{
		const Box& aBack = theInput.mBackdrop;
		double aLeftPart = std::max(0.0, (aLawn.mX - aBack.mX) * aScale);
		aLawnX = std::max(0.0, std::min(aLawnX, aLawnX - HOUSE_WIDTH * aScale + aLeftPart));
		double aRightEnd = aLawnX + (aBack.Right() - aLawn.mX) * aScale;
		if (aRightEnd < theInput.mScreenWidth)
			aLawnX = std::max(aLawnX, std::min(aLawnX + theInput.mScreenWidth - aRightEnd, theInput.mScreenWidth - aLawn.mWidth * aScale));
	}
	auto InPlace = [&](const Box& theSource)
	{
		return MakeTile(theSource, aLawnX + (theSource.mX - aLawn.mX) * aScale, aLawnY + (theSource.mY - aLawn.mY) * aScale, aScale);
	};

	size_t aFirstBackdrop = theTiles.size();
	if (!theInput.mBackdrop.Empty())
	{
		const Box& aBack = theInput.mBackdrop;
		double aLeft = std::max(aBack.mX, aLawn.mX - aLawnX / aScale);
		double aTop = std::max(aBack.mY, aLawn.mY - aLawnY / aScale);
		double aRight = std::min(aBack.Right(), aLawn.mX + (theInput.mScreenWidth - aLawnX) / aScale);
		double aBottom = std::min(aBack.Bottom(), aLawn.mY + (theInput.mScreenHeight - aLawnY) / aScale);
		if (aRight > aLeft && aBottom > aTop)
		{
			double aLeftEnd = std::min(aLawn.mX, aRight);
			if (aLeft < aLeftEnd)
			{
				Tile aTile = InPlace(Box{ aLeft, aTop, aLeftEnd - aLeft, aBottom - aTop });
				aTile.mKind = Kind::Backdrop;
				theTiles.push_back(aTile);
			}
			if (aRight > aLawn.Right())
			{
				double aFrom = std::max(aLawn.Right(), aLeft);
				Tile aTile = InPlace(Box{ aFrom, aTop, aRight - aFrom, aBottom - aTop });
				aTile.mKind = Kind::Backdrop;
				theTiles.push_back(aTile);
			}

			// past the image's ends, where it does not reach the window's edges: its edge going on
			double aCoveredLeft = aLawnX, aCoveredRight = aLawnX + aLawn.mWidth * aScale;
			for (size_t i = aFirstBackdrop; i < theTiles.size(); i++)
			{
				aCoveredLeft = std::min(aCoveredLeft, theTiles[i].mDest.mX);
				aCoveredRight = std::max(aCoveredRight, theTiles[i].mDest.Right());
			}
			Tile anImage = InPlace(aBack);
			if (aCoveredLeft > 0.5)
				theTiles.push_back(Tile{ Box{ aBack.mX, aBack.mY, 1, aBack.mHeight }, Box{ 0, anImage.mDest.mY, aCoveredLeft, anImage.mDest.mHeight }, Kind::Edge });
			if (aCoveredRight < theInput.mScreenWidth - 0.5)
				theTiles.push_back(Tile{ Box{ aBack.Right() - 1, aBack.mY, 1, aBack.mHeight },
					Box{ aCoveredRight, anImage.mDest.mY, theInput.mScreenWidth - aCoveredRight, anImage.mDest.mHeight }, Kind::Edge });
		}
	}
	size_t aEndBackdrop = theTiles.size();
	if (theInput.mBackdrop.Empty() && theInput.mSurround != SURROUND_NONE)
		theTiles.push_back(Tile{ aLawn, InPlace(aLawn).mDest, Kind::Surround, theInput.mSurround });
	if (theInput.mBeyond && aEndBackdrop > aFirstBackdrop)
		theTiles.push_back(Tile{ aLawn, InPlace(aLawn).mDest, Kind::Beyond });
	theTiles.push_back(InPlace(aLawn));
	if (theInput.mDarkWeather)
	{
		// (over the backdrop and its edges going on; the source where the screen's scale puts it)
		for (size_t i = aFirstBackdrop; i < aEndBackdrop; i++)
		{
			const Box& d = theTiles[i].mDest;
			theTiles.push_back(Tile{ Box{ aLawn.mX + (d.mX - aLawnX) / aScale, aLawn.mY + (d.mY - aLawnY) / aScale, d.mWidth / aScale, d.mHeight / aScale }, d,
				Kind::Weather });
		}
	}
	if (theInput.mBeyond)
		theTiles.push_back(Tile{ aLawn, InPlace(aLawn).mDest, Kind::Band });
	theScale = aScale;
	theLawnX = aLawnX;
	theLawnY = aLawnY;
	return true;
}

// The scale of the cards: as large as the phone versions have them, and smaller when theSlots of them would not fit down
// the window (past the home indicator, as the phone versions have them) or left of the game's screen (at theLawnX; on a
// conveyor belt thePad card units either side of them); and the right edge of their column (of the belt). False when
// there is no room for them.
bool CardColumn(const Input& theInput, double theLawnX, double theSlots, double thePad, bool theAny, double& theScale, double& theRight)
{
	const Box& aSafe = theInput.mSafeArea;
	double aScale = MAX_CARD_HEIGHT * theInput.mScreenHeight / CARD_HEIGHT;
	if (theSlots > 0)
		aScale = std::min(aScale, (theInput.mScreenHeight - aSafe.mY - 2 * EDGE_MARGIN - (theSlots - 1) * CARD_GAP) / (theSlots * CARD_HEIGHT));
	aScale = std::min(aScale, (theLawnX - GAP - aSafe.mX - EDGE_MARGIN) / (CARD_WIDTH + 2 * thePad));
	if (aScale <= 0)
		return false;
	theScale = aScale;
	theRight = aSafe.mX + EDGE_MARGIN + (theAny ? (CARD_WIDTH + 2 * thePad) * aScale : 0);
	return true;
}

// The board the cards lie on, from theLeft (the safe area's left edge) to theRight, all its height, at theScale (its
// units, the cards'): just the cards' width; over where the backdrop's image ends short of the window's left edge,
// instead of its edge going on there (and the weather's darkness over that), and left of it, under the notch or the
// Dynamic Island, plain black (all of it left of the board with theBlackLeft, where no picture is); after the other
// tiles, as the cards are. Nothing when theRight is not on the window.
void PutBoard(std::vector<Tile>& theTiles, const Input& theInput, double theLeft, double theRight, double theScale, bool theBlackLeft = false)
{
	if (theRight < 0.5)
		return;
	theLeft = std::clamp(theLeft, 0.0, theRight);
	double anImageLeft = 0;
	std::vector<Box> aCovered;
	std::erase_if(theTiles, [&](const Tile& t)
	{
		bool aCover = t.mKind == Kind::Edge && t.mDest.mX < 0.5 && t.mDest.Right() <= theRight + 1e-6;
		if (aCover)
		{
			aCovered.push_back(t.mDest);
			anImageLeft = t.mDest.Right();
		}
		return aCover;
	});
	std::erase_if(theTiles, [&](const Tile& t)
	{
		return t.mKind == Kind::Weather && std::any_of(aCovered.begin(), aCovered.end(), [&](const Box& b)
		{
			return std::abs(b.mX - t.mDest.mX) < 1e-6 && std::abs(b.mWidth - t.mDest.mWidth) < 1e-6;
		});
	});
	double aBlack = theBlackLeft ? theLeft : std::min(theLeft, anImageLeft);
	if (aBlack > 0.5)
		theTiles.push_back(Tile{ Box{ 0, 0, aBlack, theInput.mScreenHeight }, Box{ 0, 0, aBlack, theInput.mScreenHeight }, Kind::Board, 2 });
	theTiles.push_back(Tile{ Box{ 0, 0, (theRight - theLeft) / theScale, theInput.mScreenHeight / theScale },
		Box{ theLeft, 0, theRight - theLeft, theInput.mScreenHeight }, Kind::Board, 0 });
}

// A board as the cards' from theLeft to the window's right edge, its rim on its left (facing the game's screen): beside a
// picture of the board's own, which ends at the screen's edges (a garden's); nothing when there is no room
void PutRightBoard(std::vector<Tile>& theTiles, const Input& theInput, double theLeft, double theScale)
{
	double aWidth = theInput.mScreenWidth - theLeft;
	if (aWidth < 0.5)
		return;
	theTiles.push_back(Tile{ Box{ 0, 0, aWidth / theScale, theInput.mScreenHeight / theScale }, Box{ theLeft, 0, aWidth, theInput.mScreenHeight }, Kind::Board, 1 });
}

}

std::vector<Tile> Compute(const Input& theInput)
{
	std::vector<Tile> aTiles;
	const Box& aSafe = theInput.mSafeArea;
	const Box& aLawn = theInput.mLawn;
	if (theInput.mScreenHeight <= 0 || theInput.mScreenWidth / theInput.mScreenHeight < MIN_ASPECT || aSafe.Empty() || aLawn.Empty())
		return aTiles;

	// the game's screen, which the game draws without the seed bank and what the layout puts elsewhere, over the backdrop
	double aScale = 0, aLawnX = 0, aLawnY = 0;
	if (!PlaceScreen(theInput, aTiles, aScale, aLawnX, aLawnY))
		return {};
	auto InPlace = [&](const Box& theSource)
	{
		return MakeTile(theSource, aLawnX + (theSource.mX - aLawn.mX) * aScale, aLawnY + (theSource.mY - aLawn.mY) * aScale, aScale);
	};

	// The cards, at their scale (CardColumn); on a conveyor belt they ride up a belt as long as the window, with room for
	// CONVEYOR_SLOTS of them. Under them the board they lie on, from the window's left edge past them.
	size_t n = theInput.mPackets.size();
	bool aConveyor = !theInput.mConveyorBelt.Empty() && theInput.mPacketSlots.size() == n;
	double aSlots = aConveyor ? CONVEYOR_SLOTS : static_cast<double>(n);
	double aPad = aConveyor ? CONVEYOR_PAD : 0;
	double aCardScale = 0, aColumnRight = 0;
	if (!CardColumn(theInput, aLawnX, aSlots, aPad, n > 0 || aConveyor, aCardScale, aColumnRight))
		return {};
	// (just the cards' width, from the safe area's left edge, and on to where the backdrop's image begins if that is
	// further; without that picture, on to the game's screen)
	double aBoardRight = theInput.mBackdrop.Empty() ? aLawnX :
		std::min(std::max(aColumnRight + EDGE_MARGIN, aLawnX + (theInput.mBackdrop.mX - aLawn.mX) * aScale), aLawnX);
	if (theInput.mBackdrop.Empty())
	{
		PutBoard(aTiles, theInput, aSafe.mX, aLawnX, aCardScale, true);
		PutRightBoard(aTiles, theInput, aLawnX + aLawn.mWidth * aScale, aCardScale);
		std::erase_if(aTiles, [](const Tile& t) { return t.mKind == Kind::Surround; });
	}
	else
		PutBoard(aTiles, theInput, aSafe.mX, aBoardRight, aCardScale);

	// Along the top, right of the cards (and of the board's rim): the sun counter, then the shovel, all as high as the sun
	// counter
	double aBarX = theInput.mBackdrop.Empty() ? aColumnRight + GAP : std::max(aColumnRight + GAP, aBoardRight + 1);
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

std::vector<Tile> ComputeFramed(const Input& theInput)
{
	std::vector<Tile> aTiles;
	const Box& aSafe = theInput.mSafeArea;
	const Box& aScreen = theInput.mLawn;
	if (theInput.mScreenHeight <= 0 || theInput.mScreenWidth / theInput.mScreenHeight < MIN_ASPECT || aSafe.Empty() || aScreen.Empty())
		return aTiles;
	// under all, where nothing else covers the window: a blurred copy of the middle of the game's screen, as wide as it
	// and as high as the window's shape gives
	double aHeight = std::min(aScreen.mHeight, aScreen.mWidth * theInput.mScreenHeight / theInput.mScreenWidth);
	aTiles.push_back(Tile{ Box{ aScreen.mX, aScreen.mY + (aScreen.mHeight - aHeight) / 2, aScreen.mWidth, aHeight },
		Box{ 0, 0, theInput.mScreenWidth, theInput.mScreenHeight }, Kind::Blur });
	if (!theInput.mBackdrop.Empty())
	{
		// a board behind: where the levels have the game's screen, over the backdrop; the board the cards lie on where a
		// level has it, moved with the backdrop when the board pans (off the window's edge when it pans to the street), and
		// not over the game's screen
		double aScale = 0, aX = 0, aY = 0;
		if (!PlaceScreen(theInput, aTiles, aScale, aX, aY))
			return {};
		// (panned to the street, the board slides off with the house; panned to the house, as in the level)
		double anImageLeft = aX + (theInput.mBackdrop.mX - aScreen.mX) * aScale;
		double aLevelX = aSafe.Right() - aScreen.mWidth * aScale, aLevelImageLeft = aLevelX - HOUSE_WIDTH * aScale;
		double aShift = theInput.mBackdrop.mX - aScreen.mX < -HOUSE_WIDTH ? anImageLeft - aLevelImageLeft : 0;
		double aCardScale = 0, aColumnRight = 0;
		if (CardColumn(theInput, aLevelX, theInput.mCardSlots, theInput.mCardPad, true, aCardScale, aColumnRight))
			PutBoard(aTiles, theInput, aSafe.mX + aShift, std::min(std::max(aColumnRight + EDGE_MARGIN, aLevelImageLeft) + aShift, aX), aCardScale);
		return aTiles;
	}
	double aScale = std::min(theInput.mScreenHeight / aScreen.mHeight, aSafe.mWidth / aScreen.mWidth);
	double aX = aSafe.mX + (aSafe.mWidth - aScreen.mWidth * aScale) / 2;
	if (theInput.mSurround == SURROUND_STORE)	// Crazy Dave and his car together in the middle
		aX = std::clamp(aSafe.mX + aSafe.mWidth / 2 - (STORE_MIDDLE - aScreen.mX) * aScale, aSafe.mX, aSafe.Right() - aScreen.mWidth * aScale);
	Tile aTile = MakeTile(aScreen, aX, (theInput.mScreenHeight - aScreen.mHeight * aScale) / 2, aScale);
	if (theInput.mSurround == SURROUND_GARDEN)
	{
		// a garden, its picture ending at the screen's edges: boards as the cards' at both sides, their rims facing it
		double aCardScale = 0, aColumnRight = 0;
		if (CardColumn(theInput, aSafe.Right() - aScreen.mWidth * aScale, 6, 0, true, aCardScale, aColumnRight))
		{
			PutBoard(aTiles, theInput, aSafe.mX, aTile.mDest.mX, aCardScale, true);
			PutRightBoard(aTiles, theInput, aTile.mDest.Right(), aCardScale);
		}
	}
	else if (theInput.mSurround != SURROUND_NONE)
		aTiles.push_back(Tile{ aTile.mSource, aTile.mDest, Kind::Surround, theInput.mSurround });
	aTiles.push_back(aTile);
	if (theInput.mTitle)
		aTiles.push_back(Tile{ aTile.mSource, aTile.mDest, Kind::Title });
	if (theInput.mSurround == SURROUND_STORE)
		aTiles.push_back(Tile{ aTile.mSource, aTile.mDest, Kind::StoreFront });
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
