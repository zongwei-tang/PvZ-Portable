/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Tests of the phone layout (src/Lawn/System/PhoneLayout.h): the tiles stay on the screen and out of the notch, the
 * controls do not overlap and are where the phone versions have them (the cards down the left, the sun counter and the
 * shovel along the top, the progress meter and the menu button top right), nothing is black right of the safe area's
 * left edge, and taps map back to the right place of the game's screen.
 * Built with -DPHONE_LAYOUT_TESTS=ON as phone-layout-test.
 */

#include "PhoneLayout.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
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
	anInput.mLawn = Box{ 0, 0, 800, 600 };
	anInput.mBackdrop = Box{ -220, 0, 1400, 600 };
	anInput.mMenuButton = Box{ 681, 0, 117, 36 };
	anInput.mSunCounter = Box{ 10, 0, 80, 87 };
	anInput.mMeter = Box{ 580, 555, 180, 47 };
	if (theShovel)
		anInput.mShovel = Box{ static_cast<double>(anExtra + 456), 0, 70, 72 };
	for (int i = 0; i < theNumPackets; i++)
	{
		int x = theNumPackets <= 7 ? i * 59 + 85 : theNumPackets == 8 ? i * 54 + 81 : theNumPackets == 9 ? i * 52 + 80 : i * 51 + 79;
		anInput.mPackets.push_back(Box{ 10.0 + x, 8, 50, 70 });
	}
	return anInput;
}

// the tile a tap at a window point goes to: the topmost one there that is not the backdrop
int TopmostAt(const std::vector<Tile>& theTiles, double theX, double theY)
{
	for (int i = static_cast<int>(theTiles.size()) - 1; i >= 0; i--)
		if (theTiles[i].mKind != Kind::Backdrop && theTiles[i].mDest.Contains(theX, theY))
			return i;
	return -1;
}

void CheckLayout(const std::string& theName, const Input& theInput)
{
	std::printf("%s\n", theName.c_str());
	std::vector<Tile> aTiles = Compute(theInput);
	Check(!aTiles.empty(), theName + ": has a layout");
	if (aTiles.empty())
		return;
	const Box& aSafe = theInput.mSafeArea;
	Box aScreen{ 0, 0, theInput.mScreenWidth, theInput.mScreenHeight };

	// the order: the backdrop, the game's screen in one or two pieces, then the controls
	size_t aFirstLawn = 0;
	while (aFirstLawn < aTiles.size() && aTiles[aFirstLawn].mKind == Kind::Backdrop)
		aFirstLawn++;
	Check(theInput.mBackdrop.Empty() ? aFirstLawn == 0 : (aFirstLawn == 1 || aFirstLawn == 2), theName + ": a backdrop exactly when the level has one");
	for (size_t i = aFirstLawn; i < aTiles.size(); i++)
		Check(aTiles[i].mKind != Kind::Backdrop, theName + ": the backdrop comes first only");
	Check(aFirstLawn < aTiles.size() && aTiles[aFirstLawn].mKind == Kind::Screen && Same(aTiles[aFirstLawn].mSource, theInput.mLawn),
		theName + ": the game's screen, whole, after the backdrop");
	size_t aFirstControl = aFirstLawn + 1;
	for (size_t i = aFirstControl; i < aTiles.size(); i++)
		Check(aTiles[i].Overlay(), theName + ": after the game's screen only what is drawn into the window");
	const Tile& aRight = aTiles[aFirstLawn];
	double aLawnScale = aRight.mDest.mHeight / aRight.mSource.mHeight;
	double aLawnOffsetX = aRight.mDest.mX - aRight.mSource.mX * aLawnScale, aLawnOffsetY = aRight.mDest.mY - aRight.mSource.mY * aLawnScale;

	for (size_t i = 0; i < aTiles.size(); i++)
	{
		const Tile& aTile = aTiles[i];
		std::string aWhich = theName + ": tile " + std::to_string(i);
		double aScaleX = aTile.mDest.mWidth / aTile.mSource.mWidth;
		double aScaleY = aTile.mDest.mHeight / aTile.mSource.mHeight;
		if (aTile.mKind == Kind::Backdrop)
		{
			// in line with the lawn: the same scale and place, but for its part left of the lawn, which is stretched
			// across to the safe area
			Check(std::abs(aScaleY - aLawnScale) < 1e-9, aWhich + " (the backdrop) is as high as the lawn");
			Check(std::abs(aTile.mDest.mY - (aLawnOffsetY + aTile.mSource.mY * aLawnScale)) < EPSILON, aWhich + " (the backdrop) lines up with the lawn");
			if (std::abs(aTile.mSource.Right() - theInput.mLawn.mX) < EPSILON)
			{
				Check(std::abs(aTile.mSource.Right() - theInput.mLawn.mX) < EPSILON, aWhich + " (the stretched backdrop) ends where the lawn begins");
				Check(std::abs(aTile.mDest.Right() - (aLawnOffsetX + theInput.mLawn.mX * aLawnScale)) < EPSILON, aWhich + " (the stretched backdrop) meets the lawn");
				Check(std::abs(aTile.mDest.mX - std::max(0.0, aSafe.mX)) < EPSILON, aWhich + " (the stretched backdrop) reaches the safe area");
				Check(aScaleX >= aLawnScale - 1e-9 && aScaleX < aLawnScale * 2, aWhich + " (the stretched backdrop) is stretched less than twice");
			}
			else
			{
				Check(std::abs(aTile.mSource.mX - theInput.mLawn.Right()) < EPSILON, aWhich + " (the backdrop) is right of the lawn");
				Check(std::abs(aScaleX - aLawnScale) < 1e-9, aWhich + " (the backdrop) has the scale of the lawn");
				Check(std::abs(aTile.mDest.mX - (aLawnOffsetX + aTile.mSource.mX * aLawnScale)) < EPSILON, aWhich + " (the backdrop) lines up with the lawn across");
			}
			continue;
		}
		if (aTile.mKind == Kind::Screen)
			Check(std::abs(aScaleX - aScaleY) < 1e-9, aWhich + " is scaled alike both ways");
		Check(Inside(aTile.mDest, aScreen), aWhich + " is on the screen");
		// nothing goes under the notch or the Dynamic Island
		Check(aTile.mDest.mX >= aSafe.mX - EPSILON && aTile.mDest.Right() <= aSafe.Right() + EPSILON, aWhich + " is clear of the notch");
		if (i >= aFirstControl)
		{
			// the controls stay clear of the top inset too (the cards may go down past the home indicator), do not
			// overlap one another, and are on top where they are
			Check(aTile.mDest.mY >= aSafe.mY - EPSILON, aWhich + " (a control) is below the top of the safe area");
			for (size_t j = i + 1; j < aTiles.size(); j++)
				Check(!Overlap(aTile.mDest, aTiles[j].mDest), aWhich + " does not overlap tile " + std::to_string(j));
		}

		// a tap anywhere in it: to the same place of the game's screen (for a card, of its packet), or to nothing for the
		// sun counter and the progress meter
		for (double fx : { 0.01, 0.5, 0.99 })
		{
			for (double fy : { 0.01, 0.5, 0.99 })
			{
				double x = aTile.mDest.mX + aTile.mDest.mWidth * fx;
				double y = aTile.mDest.mY + aTile.mDest.mHeight * fy;
				int aTop = TopmostAt(aTiles, x, y);
				if (i >= aFirstControl)
					Check(aTop == static_cast<int>(i), aWhich + " (a control) is on top where it is");
				if (aTop < 0)
					continue;
				const Tile& aHit = aTiles[aTop];
				double aGameX = -1, aGameY = -1;
				bool aMapped = WindowToGame(aTiles, x, y, aGameX, aGameY);
				if (aHit.mKind == Kind::SunCounter || aHit.mKind == Kind::Meter)
				{
					Check(!aMapped, aWhich + ": a tap on the sun counter or the progress meter goes nowhere");
					continue;
				}
				Check(aMapped, aWhich + " is hit by a tap in it");
				Check(std::abs(aGameX - (aHit.mSource.mX + (x - aHit.mDest.mX) / aHit.mDest.mWidth * aHit.mSource.mWidth)) < 1e-6 &&
					std::abs(aGameY - (aHit.mSource.mY + (y - aHit.mDest.mY) / aHit.mDest.mHeight * aHit.mSource.mHeight)) < 1e-6,
					aWhich + " maps a tap back");
				Check(aHit.mSource.Contains(aGameX, aGameY) || fx > 0.9 || fy > 0.9, aWhich + " maps a tap into what it shows");
			}
		}
	}

	// every cell of the lawn shown and not under a control
	for (int aGridX = 0; aGridX < 9; aGridX++)
	{
		for (int aGridY = 0; aGridY < 6; aGridY++)
		{
			// a cell of the lawn (rows of 100 on day and night, 85 on the pool and roof): its middle and its top corners
			for (double aRowHeight : { 85.0, 100.0 })
			{
				double aCellTop = 80 + aGridY * aRowHeight;
				if (aCellTop + aRowHeight / 2 >= 600)
					continue;
				for (auto [aCellX, aCellY] : { std::pair{ 40 + aGridX * 80 + 40.0, aCellTop + aRowHeight / 2 },
					std::pair{ 40 + aGridX * 80 + 8.0, aCellTop + 8 }, std::pair{ 40 + aGridX * 80 + 72.0, aCellTop + 8 } })
				{
					bool aShown = false;
					for (size_t i = aFirstLawn; i < aFirstControl; i++)
					{
						const Tile& t = aTiles[i];
						if (!t.mSource.Contains(aCellX, aCellY))
							continue;
						double x = t.mDest.mX + (aCellX - t.mSource.mX) * aLawnScale;
						double y = t.mDest.mY + (aCellY - t.mSource.mY) * aLawnScale;
						aShown = aShown || TopmostAt(aTiles, x, y) == static_cast<int>(i);
					}
					Check(aShown, theName + ": cell " + std::to_string(aGridX) + "," + std::to_string(aGridY) + " is shown and tappable");
				}
			}
		}
	}

	// the controls
	auto aFindKind = [&](Kind theKind) -> const Tile*
	{
		for (size_t i = aFirstControl; i < aTiles.size(); i++)
			if (aTiles[i].mKind == theKind)
				return &aTiles[i];
		return nullptr;
	};

	// the cards: one for each packet, in order, in one column at the left edge of the safe area from its top, each
	// below the one before, all alike, as wide for their height as the phone versions have them
	std::vector<const Tile*> aCards;
	for (const Tile& t : aTiles)
		if (t.mKind == Kind::Card)
			aCards.push_back(&t);
	Check(aCards.size() == theInput.mPackets.size(), theName + ": a card for each packet");
	double aCardHeight = 0, aCardWidth = 0, aColumnRight = aSafe.mX;
	for (size_t i = 0; i < aCards.size(); i++)
	{
		const Tile& t = *aCards[i];
		std::string aWhich = theName + ": card " + std::to_string(i);
		Check(t.mIndex == static_cast<int>(i) && Same(t.mSource, theInput.mPackets[i]), aWhich + " is of its packet");
		Check(std::abs(t.mDest.mX - (aSafe.mX + EDGE_MARGIN)) < EPSILON, aWhich + " is at the left edge of the safe area");
		Check(std::abs(t.mDest.mWidth / t.mDest.mHeight - CARD_WIDTH / CARD_HEIGHT) < 1e-9, aWhich + " is a wide card");
		if (i == 0)
		{
			Check(std::abs(t.mDest.mY - (aSafe.mY + EDGE_MARGIN)) < EPSILON, aWhich + " is at the top");
			aCardHeight = t.mDest.mHeight;
			aCardWidth = t.mDest.mWidth;
			aColumnRight = t.mDest.Right();
		}
		else
		{
			Check(std::abs(t.mDest.mHeight - aCardHeight) < EPSILON, aWhich + " is as large as the others");
			Check(std::abs(t.mDest.mY - (aCards[i - 1]->mDest.Bottom() + CARD_GAP)) < EPSILON, aWhich + " is below the one before");
		}
	}
	if (!aCards.empty())
	{
		double aLetterboxScale = std::min(theInput.mScreenWidth / 800, theInput.mScreenHeight / 600);
		Check(aCardWidth > 50 * aLetterboxScale * 1.2, theName + ": cards are wider than packets letterboxed");
		Check(aCardHeight >= 30, theName + ": cards are at least 30 points high");
		Check(aCardHeight <= MAX_CARD_HEIGHT * theInput.mScreenHeight + EPSILON, theName + ": cards are not larger than the phone versions have them");
		Check(aCards.back()->mDest.Bottom() <= theInput.mScreenHeight + EPSILON, theName + ": the cards fit down the window");
	}

	// along the top: the sun counter right of the cards, then the shovel, as high as it; the menu button in the top
	// right corner; the progress meter between the shovel and the button
	const Tile* aSun = aFindKind(Kind::SunCounter);
	Check(aSun != nullptr, theName + ": the sun counter is shown");
	double aBarRight = aColumnRight;
	if (aSun)
	{
		Check(aSun->mDest.mX > aColumnRight && aSun->mDest.mX < aColumnRight + 2 * GAP, theName + ": the sun counter is right beside the cards");
		Check(std::abs(aSun->mDest.mY - (aSafe.mY + EDGE_MARGIN)) < EPSILON, theName + ": the sun counter is at the top");
		Check(aSun->mDest.mWidth > aSun->mDest.mHeight * 2, theName + ": the sun counter is a wide bar");
		aBarRight = aSun->mDest.Right();
	}
	if (!theInput.mShovel.Empty())
	{
		const Tile* aShovel = aFindKind(Kind::Shovel);
		Check(aShovel != nullptr, theName + ": the shovel has a tile");
		if (aShovel && aSun)
		{
			Check(aShovel->mDest.mX > aSun->mDest.Right() && aShovel->mDest.mX < aSun->mDest.Right() + 2 * GAP, theName + ": the shovel is beside the sun counter");
			Check(Same(aShovel->mSource, theInput.mShovel), theName + ": the shovel's tile is of the shovel");
			Check(std::abs(aShovel->mDest.mY - aSun->mDest.mY) < EPSILON && std::abs(aShovel->mDest.mHeight - aSun->mDest.mHeight) < EPSILON,
				theName + ": the shovel is as high as the sun counter");
			aBarRight = aShovel->mDest.Right();
		}
	}
	const Tile* aMenu = theInput.mMenuButton.Empty() ? nullptr : aFindKind(Kind::Menu);
	if (!theInput.mMenuButton.Empty())
	{
		Check(aMenu != nullptr, theName + ": the menu button has a tile");
		if (aMenu)
		{
			Check(aMenu->mDest.Right() > aSafe.Right() - 6 && std::abs(aMenu->mDest.mY - aSafe.mY) < EPSILON, theName + ": the menu button is in the top right corner");
			Check(aMenu->mDest.mHeight >= 36, theName + ": the menu button is at least 36 points high");
			Check(aMenu->mDest.mX > aBarRight, theName + ": the menu button is right of the shovel");
			Check(Same(aMenu->mSource, theInput.mMenuButton), theName + ": the menu button's tile is of the button");
			Check(std::abs(aMenu->mDest.mWidth / aMenu->mDest.mHeight - theInput.mMenuButton.mWidth / theInput.mMenuButton.mHeight) < 1e-9,
				theName + ": the menu button keeps its aspect");
		}
	}
	const Tile* aMeter = aFindKind(Kind::Meter);
	Check((aMeter != nullptr) == !theInput.mMeter.Empty(), theName + ": the progress meter is shown when the game shows it");
	if (aMeter)
	{
		Check(aMeter->mDest.mX > aBarRight, theName + ": the progress meter is right of the shovel");
		if (aMenu)
			Check(aMeter->mDest.Right() < aMenu->mDest.mX && aMeter->mDest.Right() > aMenu->mDest.mX - 2 * GAP, theName + ": the progress meter is beside the menu button");
		Check(std::abs(aMeter->mDest.mWidth / aMeter->mDest.mHeight - theInput.mMeter.mWidth / theInput.mMeter.mHeight) < 1e-9,
			theName + ": the progress meter keeps its aspect");
	}

	// no black: right of the safe area's left edge, every point of the window shows something (with a backdrop)
	int aCovered = 0, aSamples = 0;
	for (double y = 0.5; y < theInput.mScreenHeight; y += 2)
	{
		for (double x = std::max(0.0, aSafe.mX) + 0.5; x < theInput.mScreenWidth; x += 2)
		{
			aSamples++;
			for (const Tile& aTile : aTiles)
			{
				if (aTile.mDest.Contains(x, y))
				{
					aCovered++;
					break;
				}
			}
		}
	}
	double aCoverage = static_cast<double>(aCovered) / aSamples;
	if (!theInput.mBackdrop.Empty())
		Check(aCovered == aSamples, theName + ": nothing is black right of the safe area's left edge");

	// larger than the 800x600 screen letterboxed into the window
	double aLetterboxScale = std::min(theInput.mScreenWidth / 800, theInput.mScreenHeight / 600);
	Check(aLawnScale > aLetterboxScale * 0.999, theName + ": the lawn is as large as letterboxed at the least");
	Check(std::abs(aRight.mDest.mHeight - theInput.mScreenHeight) < EPSILON || aRight.mDest.mHeight < theInput.mScreenHeight,
		theName + ": the lawn is not taller than the window");
	std::printf("  lawn %.0f%% of letterboxed, cards %.0fx%.0f pt, %.0f%% of the window right of the safe edge covered, %zu tiles\n",
		aLawnScale / aLetterboxScale * 100, aCardWidth, aCardHeight, aCoverage * 100, aTiles.size());
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
	// before the first wave, without the progress meter
	{
		Input anInput = GameInput(852, 393, aPhoneSafe, 6, true);
		anInput.mMeter = Box{};
		CheckLayout("iPhone 15, no progress meter", anInput);
	}
	// a level without a backdrop image
	{
		Input anInput = GameInput(852, 393, aPhoneSafe, 6, true);
		anInput.mBackdrop = Box{};
		CheckLayout("iPhone 15, no backdrop", anInput);
	}
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
