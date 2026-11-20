/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Tests of the phone layout (src/Lawn/System/PhoneLayout.h): the tiles stay on the screen and out of the notch, do
 * not overlap, keep their aspect, show every control whole, and taps map back to the right place of the game's screen.
 * Built with -DPHONE_LAYOUT_TESTS=ON as phone-layout-test.
 */

#include "PhoneLayout.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace PhoneLayout;

namespace
{

int gChecks = 0;
int gFailures = 0;

void Check(bool theOk, const std::string& theWhat)
{
	gChecks++;
	if (!theOk)
	{
		gFailures++;
		std::printf("  FAILED: %s\n", theWhat.c_str());
	}
}

constexpr double EPSILON = 1e-6;

bool Inside(const Box& theInner, const Box& theOuter)
{
	return theInner.mX >= theOuter.mX - EPSILON && theInner.mY >= theOuter.mY - EPSILON &&
		theInner.Right() <= theOuter.Right() + EPSILON && theInner.Bottom() <= theOuter.Bottom() + EPSILON;
}

bool Overlap(const Box& a, const Box& b)
{
	double aWidth = std::min(a.Right(), b.Right()) - std::max(a.mX, b.mX);
	double aHeight = std::min(a.Bottom(), b.Bottom()) - std::max(a.mY, b.mY);
	return aWidth > EPSILON && aHeight > EPSILON;
}

bool Same(const Box& a, const Box& b)
{
	return std::abs(a.mX - b.mX) < EPSILON && std::abs(a.mY - b.mY) < EPSILON &&
		std::abs(a.mWidth - b.mWidth) < EPSILON && std::abs(a.mHeight - b.mHeight) < EPSILON;
}

// The game's screen as Board lays it out for a level with theNumPackets packets (Board::GetSeedPacketPositionX,
// GetShovelButtonRect, the menu button at (681, -10, 117, 46)), with the margins the game uses around each piece.
Input GameInput(double theScreenWidth, double theScreenHeight, Box theSafe, int theNumPackets, bool theShovel)
{
	static const int EXTRA_WIDTH[] = { 0, 0, 0, 0, 0, 0, 0, 60, 76, 112, 153 };
	int anExtra = EXTRA_WIDTH[theNumPackets];
	Input anInput;
	anInput.mScreenWidth = theScreenWidth;
	anInput.mScreenHeight = theScreenHeight;
	anInput.mSafeArea = theSafe;
	anInput.mLawn = Box{ 0, 50, 800, 550 };
	anInput.mBankBottom = 87;
	double aShovelRight = theShovel ? anExtra + 456 + 70 : 0;
	anInput.mBankRight = std::max(10.0 + 446 + anExtra, aShovelRight);
	anInput.mMenuButton = Box{ 681, 0, 117, 36 };
	anInput.mSunCounter = Box{ 10, 0, 83, 87 };
	if (theShovel)
		anInput.mShovel = Box{ static_cast<double>(anExtra + 456), 0, 70, 72 };
	for (int i = 0; i < theNumPackets; i++)
	{
		int x = theNumPackets <= 7 ? i * 59 + 85 : theNumPackets == 8 ? i * 54 + 81 : theNumPackets == 9 ? i * 52 + 80 : i * 51 + 79;
		anInput.mPackets.push_back(Box{ 10.0 + x - 2, 6, 54, 74 });
	}
	return anInput;
}

void CheckLayout(const std::string& theName, const Input& theInput)
{
	std::printf("%s\n", theName.c_str());
	std::vector<Tile> aTiles = Compute(theInput);
	Check(!aTiles.empty(), theName + ": has a layout");
	if (aTiles.empty())
		return;

	Box aScreen{ 0, 0, theInput.mScreenWidth, theInput.mScreenHeight };
	size_t aNumLawnTiles = aTiles.size() - theInput.mPackets.size() - (theInput.mMenuButton.Empty() ? 0 : 1) -
		(theInput.mSunCounter.Empty() ? 0 : 1) - (theInput.mShovel.Empty() ? 0 : 1);
	Check(aNumLawnTiles == 1 || aNumLawnTiles == 2, theName + ": the lawn is one or two tiles");

	for (size_t i = 0; i < aTiles.size(); i++)
	{
		const Tile& aTile = aTiles[i];
		std::string aWhich = theName + ": tile " + std::to_string(i);
		Check(Inside(aTile.mDest, aScreen), aWhich + " is on the screen");
		// the lawn may run under the home indicator, but nothing goes under the notch or the Dynamic Island
		Check(aTile.mDest.mX >= theInput.mSafeArea.mX - EPSILON && aTile.mDest.Right() <= theInput.mSafeArea.Right() + EPSILON,
			aWhich + " is clear of the notch");
		if (i >= aNumLawnTiles)
			Check(Inside(aTile.mDest, theInput.mSafeArea), aWhich + " (a control) is inside the safe area");
		double aScaleX = aTile.mDest.mWidth / aTile.mSource.mWidth;
		double aScaleY = aTile.mDest.mHeight / aTile.mSource.mHeight;
		Check(std::abs(aScaleX - aScaleY) < 1e-9, aWhich + " is scaled alike both ways");
		for (size_t j = i + 1; j < aTiles.size(); j++)
			Check(!Overlap(aTile.mDest, aTiles[j].mDest), aWhich + " does not overlap tile " + std::to_string(j));

		// a tap anywhere in the tile lands in the same place of the game's screen
		for (double fx : { 0.01, 0.5, 0.99 })
		{
			for (double fy : { 0.01, 0.5, 0.99 })
			{
				double x = aTile.mDest.mX + aTile.mDest.mWidth * fx;
				double y = aTile.mDest.mY + aTile.mDest.mHeight * fy;
				double aGameX = -1, aGameY = -1;
				bool aHit = WindowToGame(aTiles, x, y, aGameX, aGameY);
				Check(aHit, aWhich + " is hit by a tap in it");
				Check(std::abs(aGameX - (aTile.mSource.mX + aTile.mSource.mWidth * fx)) < 1e-6 &&
					std::abs(aGameY - (aTile.mSource.mY + aTile.mSource.mHeight * fy)) < 1e-6, aWhich + " maps a tap back");
			}
		}
	}

	// the lawn: one scale, its pieces side by side, the whole lawn shown except the corner the bank took
	const Tile& aRight = aTiles[0];
	double aLawnScale = aRight.mDest.mHeight / aRight.mSource.mHeight;
	if (aNumLawnTiles == 2)
	{
		const Tile& aLeft = aTiles[1];
		Check(std::abs(aLeft.mDest.mWidth / aLeft.mSource.mWidth - aLawnScale) < 1e-9, theName + ": the lawn pieces have one scale");
		Check(std::abs(aLeft.mDest.Right() - aRight.mDest.mX) < EPSILON, theName + ": the lawn pieces meet");
		Check(std::abs(aLeft.mDest.Bottom() - aRight.mDest.Bottom()) < EPSILON, theName + ": the lawn pieces end alike");
	}
	for (int aGridX = 0; aGridX < 9; aGridX++)
	{
		for (int aGridY = 0; aGridY < 6; aGridY++)
		{
			// the centre of each cell of the lawn (rows of 100 on day and night, 85 on the pool and roof)
			for (double aRowHeight : { 85.0, 100.0 })
			{
				double aCellX = 40 + aGridX * 80 + 40;
				double aCellY = 80 + aGridY * aRowHeight + aRowHeight / 2;
				if (aCellY >= 600)
					continue;
				bool aShown = false;
				for (size_t i = 0; i < aNumLawnTiles; i++)
					aShown = aShown || aTiles[i].mSource.Contains(aCellX, aCellY);
				Check(aShown, theName + ": cell " + std::to_string(aGridX) + "," + std::to_string(aGridY) + " is shown");
			}
		}
	}

	// every control is shown whole, in a tile of its own
	auto aShownWhole = [&](const Box& theBox, const std::string& theWhat)
	{
		bool aFound = false;
		for (size_t i = aNumLawnTiles; i < aTiles.size(); i++)
			aFound = aFound || Same(aTiles[i].mSource, theBox);
		Check(aFound, theName + ": " + theWhat + " has a tile");
	};
	if (!theInput.mMenuButton.Empty())
		aShownWhole(theInput.mMenuButton, "the menu button");
	aShownWhole(theInput.mSunCounter, "the sun counter");
	if (!theInput.mShovel.Empty())
		aShownWhole(theInput.mShovel, "the shovel");
	double aSmallestPacket = 1e9;
	for (size_t i = 0; i < theInput.mPackets.size(); i++)
	{
		aShownWhole(theInput.mPackets[i], "packet " + std::to_string(i));
		for (const Tile& aTile : aTiles)
			if (Same(aTile.mSource, theInput.mPackets[i]))
				aSmallestPacket = std::min(aSmallestPacket, aTile.mDest.mWidth);
	}

	// larger than the 800x600 screen letterboxed into the window, and packets big enough to tap
	double aLetterboxScale = std::min(theInput.mScreenWidth / 800, theInput.mScreenHeight / 600);
	Check(aLawnScale > aLetterboxScale * 1.05, theName + ": the lawn is larger than letterboxed");
	if (!theInput.mPackets.empty())
	{
		double aLetterboxPacket = 54 * aLetterboxScale;
		Check(aSmallestPacket > aLetterboxPacket, theName + ": packets are larger than letterboxed");
		Check(aSmallestPacket >= 36, theName + ": packets are at least 36 points wide");
	}
	std::printf("  lawn %.0f%% of letterboxed, packets %.0f pt wide (letterboxed %.0f pt), %zu tiles\n",
		aLawnScale / aLetterboxScale * 100, aSmallestPacket, 54 * aLetterboxScale, aTiles.size());
}

}

int main()
{
	// iPhone 15 and 16 in landscape: 852x393 points, the Dynamic Island on one side; iOS gives both sides the same inset
	Box aPhoneSafe{ 59, 0, 852 - 118, 393 - 21 };
	for (int aPackets : { 1, 6, 7, 8, 9, 10 })
	{
		for (bool aShovel : { true, false })
			CheckLayout("iPhone 15, " + std::to_string(aPackets) + " packets" + (aShovel ? ", shovel" : ""),
				GameInput(852, 393, aPhoneSafe, aPackets, aShovel));
	}
	// the other way round, with the inset only on the side of the island
	CheckLayout("iPhone 15, island on the left only", GameInput(852, 393, Box{ 59, 0, 852 - 59, 372 }, 10, true));
	CheckLayout("iPhone 15, island on the right only", GameInput(852, 393, Box{ 0, 0, 852 - 59, 372 }, 10, true));
	// iPhone 15 Pro Max, iPhone SE (16:9, no notch), iPhone 13 mini
	CheckLayout("iPhone 15 Pro Max", GameInput(932, 430, Box{ 59, 0, 932 - 118, 430 - 21 }, 10, true));
	CheckLayout("iPhone SE", GameInput(667, 375, Box{ 0, 0, 667, 375 }, 10, true));
	CheckLayout("iPhone 13 mini", GameInput(812, 375, Box{ 50, 0, 812 - 100, 375 - 21 }, 8, true));

	// screens that are not wide keep the 800x600 screen
	std::printf("narrow screens\n");
	Check(Compute(GameInput(1180, 820, Box{ 0, 0, 1180, 800 }, 6, true)).empty(), "iPad keeps the 800x600 screen");
	Check(Compute(GameInput(800, 600, Box{ 0, 0, 800, 600 }, 6, true)).empty(), "a 4:3 window keeps the 800x600 screen");
	Check(Compute(GameInput(1280, 800, Box{ 0, 0, 1280, 800 }, 6, true)).empty(), "a 16:10 window keeps the 800x600 screen");

	// a tap outside every tile hits nothing
	std::vector<Tile> aTiles = Compute(GameInput(852, 393, aPhoneSafe, 6, true));
	double x = 0, y = 0;
	Check(!WindowToGame(aTiles, 10, 10, x, y), "a tap under the Dynamic Island hits nothing");

	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
