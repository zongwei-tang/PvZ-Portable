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
#include <tuple>
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
		if (aTile.mKind == Kind::Backdrop && aTile.mMirrored)
		{
			// mirrored past an end of the image: its end at the mirror, as high as the lawn
			Check(std::abs(aScaleY - aLawnScale) < 1e-9 && std::abs(aScaleX - aLawnScale) < 1e-9, aWhich + " (the mirrored backdrop) has the lawn's scale");
			Check(std::abs(aTile.mSource.mX - theInput.mBackdrop.mX) < EPSILON || std::abs(aTile.mSource.Right() - theInput.mBackdrop.Right()) < EPSILON,
				aWhich + " (the mirrored backdrop) is at an end of the image");
			continue;
		}
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
				Check(std::abs(aTile.mDest.mX - std::max(0.0, aTile.mDest.Right() - 2 * aTile.mSource.mWidth * aLawnScale)) < EPSILON,
					aWhich + " (the stretched backdrop) reaches the window's left edge, or is twice as wide");
				Check(aScaleX >= aLawnScale - 1e-9 && aScaleX <= aLawnScale * 2 + 1e-9, aWhich + " (the stretched backdrop) is stretched twice at the most");
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

// A conveyor belt level: the belt down the left edge of the safe area as long as the window, each card over it where its
// packet is along the belt (those past the window's bottom not shown), no sun counter, taps on a card to its packet and
// on the belt to nothing
void CheckConveyor(const std::string& theName, double theWidth, double theHeight, Box theSafe, const std::vector<double>& theSlots)
{
	std::printf("%s\n", theName.c_str());
	Input anInput = GameInput(theWidth, theHeight, theSafe, 0, true);
	anInput.mSunCounter = Box{};
	anInput.mConveyorBelt = Box{ 93, 0, 516, 86 };
	for (size_t i = 0; i < theSlots.size(); i++)
	{
		anInput.mPackets.push_back(Box{ 91 + 10 + theSlots[i] * 50, 8, 50, 70 });
		anInput.mPacketSlots.push_back(theSlots[i]);
	}
	std::vector<Tile> aTiles = Compute(anInput);
	Check(!aTiles.empty(), theName + ": has a layout");
	const Tile* aBelt = nullptr;
	std::vector<const Tile*> aCards;
	for (const Tile& t : aTiles)
	{
		Check(t.mKind != Kind::SunCounter, theName + ": no sun counter");
		if (t.mKind == Kind::Conveyor)
			aBelt = &t;
		if (t.mKind == Kind::Card)
			aCards.push_back(&t);
	}
	Check(aBelt != nullptr, theName + ": the belt is shown");
	if (!aBelt)
		return;
	Check(std::abs(aBelt->mDest.mX - (theSafe.mX + EDGE_MARGIN)) < EPSILON && std::abs(aBelt->mDest.mY - theSafe.mY) < EPSILON &&
		std::abs(aBelt->mDest.Bottom() - theHeight) < EPSILON, theName + ": the belt runs down the left edge of the safe area, to the bottom");
	double aScale = aBelt->mDest.mWidth / (CARD_WIDTH + 2 * CONVEYOR_PAD);
	double aPitch = CARD_HEIGHT * aScale + CARD_GAP;
	Check(theSafe.mY + EDGE_MARGIN + (CONVEYOR_SLOTS - 1) * aPitch + CARD_HEIGHT * aScale <= theHeight + EPSILON, theName + ": the belt has room for all its slots");
	size_t aShown = 0;
	for (double aSlot : theSlots)
		aShown += theSafe.mY + EDGE_MARGIN + aSlot * aPitch < theHeight;
	Check(aCards.size() == aShown, theName + ": a card for each packet on the window");
	for (const Tile* t : aCards)
	{
		std::string aWhich = theName + ": card " + std::to_string(t->mIndex);
		double aSlot = theSlots[t->mIndex];
		Check(std::abs(t->mDest.mY - (theSafe.mY + EDGE_MARGIN + aSlot * aPitch)) < EPSILON, aWhich + " is where its packet is along the belt");
		Check(Inside(Box{ t->mDest.mX, t->mDest.mY, t->mDest.mWidth, std::min(t->mDest.mHeight, theHeight - t->mDest.mY) }, aBelt->mDest), aWhich + " is over the belt");
		double x = 0, y = 0;
		Check(WindowToGame(aTiles, t->mDest.mX + t->mDest.mWidth / 2, t->mDest.mY + 1, x, y) && t->mSource.Contains(x, y), aWhich + ": a tap on it goes to its packet");
	}
	double x = 0, y = 0;
	Check(!WindowToGame(aTiles, aBelt->mDest.mX + 1, theHeight - 1, x, y), theName + ": a tap on the belt goes nowhere");
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

	// conveyor belts: packets at the belt's start, along it, one coming in past its end
	CheckConveyor("iPhone 15, conveyor belt", 852, 393, aPhoneSafe, { 0, 1, 2.5, 7.2, 10.3 });
	CheckConveyor("iPhone SE, conveyor belt", 667, 375, Box{ 0, 0, 667, 375 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
	CheckConveyor("iPhone 15 Pro Max, conveyor belt", 932, 430, Box{ 59, 0, 932 - 118, 430 - 21 }, { 0.4, 9.9 });
	CheckConveyor("iPhone 15, empty conveyor belt", 852, 393, aPhoneSafe, {});

	// framed, outside the level layout: in the middle of the safe area as tall as the window (with the main menu's
	// scenery, or not), or with a lawn behind where the levels have the game's screen over the backdrop, wherever the
	// board is panned to; always over a blurred copy that covers the window
	for (auto [aName, w, h, aSafe] : { std::tuple{ "iPhone 15", 852.0, 393.0, aPhoneSafe }, std::tuple{ "iPhone SE", 667.0, 375.0, Box{ 0, 0, 667, 375 } },
		std::tuple{ "iPhone 15 Pro Max", 932.0, 430.0, Box{ 59, 0, 932 - 118, 430 - 21 } }, std::tuple{ "iPhone 15, island on the left only", 852.0, 393.0, Box{ 59, 0, 793, 372 } } })
	{
		for (int aCase = 0; aCase < 7; aCase++)
		{
			static const char* const CASES[] = { "", ", main menu", ", lawn behind", ", lawn panned to the street", ", lawn panned to the house", ", a page",
				", the store" };
			std::string aWhich = std::string(aName) + ", framed" + CASES[aCase];
			std::printf("%s\n", aWhich.c_str());
			Input anInput;
			anInput.mScreenWidth = w;
			anInput.mScreenHeight = h;
			anInput.mSafeArea = aSafe;
			anInput.mLawn = Box{ 0, 0, 800, 600 };
			anInput.mSurround = aCase == 1 ? SURROUND_MENU : aCase == 5 ? SURROUND_PAGE : aCase == 6 ? SURROUND_STORE : SURROUND_NONE;
			anInput.mSurroundAtRight = aCase == 1 || aCase == 6;
			if (aCase >= 2 && aCase <= 4)
				anInput.mBackdrop = Box{ aCase == 3 ? -600.0 : aCase == 4 ? 0.0 : -220.0, 0, 1400, 600 };
			std::vector<Tile> aFramed = ComputeFramed(anInput);
			Check(aFramed.size() >= 2 && aFramed[0].mKind == Kind::Blur, aWhich + ": the blurred copy first");
			if (aFramed.size() < 2)
				continue;
			Check(Same(aFramed[0].mDest, Box{ 0, 0, w, h }) && Inside(aFramed[0].mSource, Box{ 0, 0, 800, 600 }) &&
				std::abs(aFramed[0].mSource.mWidth / aFramed[0].mSource.mHeight - w / h) < 1e-9, aWhich + ": the blurred copy covers the window");
			const Tile* aScreenTile = nullptr;
			int aScenes = 0, aStores = 0;
			for (const Tile& t : aFramed)
			{
				if (t.mKind == Kind::Screen)
					aScreenTile = &t;
				aScenes += t.mKind == Kind::Surround && (t.mIndex == SURROUND_MENU || t.mIndex == SURROUND_PAGE);
				aStores += t.mKind == Kind::Surround && t.mIndex == SURROUND_STORE;
			}
			Check(aScreenTile != nullptr && Same(aScreenTile->mSource, Box{ 0, 0, 800, 600 }), aWhich + ": the game's screen, whole");
			if (!aScreenTile)
				continue;
			Check(std::abs(aScreenTile->mDest.mHeight - h) < EPSILON, aWhich + ": the game's screen is as tall as the window");
			if (aCase != 1 && aCase != 4 && aCase != 6)
				Check(aScreenTile->mDest.mX >= aSafe.mX - EPSILON && aScreenTile->mDest.Right() <= aSafe.Right() + EPSILON, aWhich + ": the game's screen is clear of the notch");
			Check(aScenes == (aCase == 1 || aCase == 5 ? 1 : 0), aWhich + ": the main menu's scenery exactly in the main menu and around pages");
			Check(aStores == (aCase == 6 ? 1 : 0), aWhich + ": the store's scenery exactly in the store");
			if (aCase == 1 || aCase == 6)
			{
				// against the window's right edge: what the picture cuts off at its right edge the phone's edge cuts off
				Check(std::abs(aScreenTile->mDest.Right() - w) < EPSILON, aWhich + ": the game's screen is against the window's right edge");
				Check(aScreenTile->mDest.mX >= aSafe.mX - EPSILON, aWhich + ": the game's screen is right of the notch");
			}
			else if (aCase >= 2 && aCase <= 4)
			{
				// as in the level layout, so that nothing moves when a dialog opens in a level
				std::vector<Tile> aLevel = Compute(GameInput(w, h, aSafe, 6, true));
				const Tile* aLevelScreen = nullptr;
				for (const Tile& t : aLevel)
					if (t.mKind == Kind::Screen && Same(t.mSource, Box{ 0, 0, 800, 600 }))
						aLevelScreen = &t;
				if (aCase != 4)
					Check(aLevelScreen && Same(aLevelScreen->mDest, aScreenTile->mDest), aWhich + ": the game's screen is where the level layout has it");
				else
					Check(aLevelScreen && aScreenTile->mDest.mX <= aLevelScreen->mDest.mX + EPSILON && aScreenTile->mDest.mX >= -EPSILON,
						aWhich + ": the game's screen is left of where the level layout has it, on the window (the board panned to the house)");
				for (const Tile& t : aFramed)
				{
					if (t.mKind != Kind::Backdrop)
						continue;
					Check(Inside(t.mSource, anInput.mBackdrop), aWhich + ": a piece of the backdrop is of the image");
					Check(!Overlap(t.mDest, aScreenTile->mDest), aWhich + ": the backdrop is beside the game's screen");
					double aScaleX = t.mDest.mWidth / t.mSource.mWidth;
					double aScale = aScreenTile->mDest.mWidth / 800;
					Check(aScaleX >= aScale - 1e-9 && aScaleX <= 2 * aScale + 1e-9, aWhich + ": the backdrop is stretched twice as wide at the most");
					Check(std::abs(t.mDest.mY - (aScreenTile->mDest.mY + (t.mSource.mY - 0) * aScale)) < EPSILON, aWhich + ": the backdrop lines up with the game's screen");
				}
				if (aCase == 2)
				{
					// the house and the street fill the window right of the safe area's left edge
					for (double x = std::max(0.0, aSafe.mX) + 0.5; x < w; x += 3)
					{
						bool aCovered = aScreenTile->mDest.Contains(x, h / 2);
						for (const Tile& t : aFramed)
							aCovered = aCovered || (t.mKind == Kind::Backdrop && t.mDest.Contains(x, h / 2));
						if (!aCovered)
						{
							Check(false, aWhich + ": the backdrop and the game's screen fill the window across");
							break;
						}
					}
				}
			}
			else
				Check(std::abs(aScreenTile->mDest.mX + aScreenTile->mDest.mWidth / 2 - (aSafe.mX + aSafe.mWidth / 2)) < EPSILON, aWhich + ": the game's screen is in the middle of the safe area");
			double x = 0, y = 0;
			Check(WindowToGame(aFramed, aScreenTile->mDest.mX + aScreenTile->mDest.mWidth / 2, h / 2, x, y) && std::abs(x - 400) < 1e-6 && std::abs(y - 300) < 1e-6,
				aWhich + ": a tap in the middle of the game's screen goes there");
			if (aScreenTile->mDest.mX > 2)
				Check(!WindowToGame(aFramed, aScreenTile->mDest.mX / 2, h / 2, x, y), aWhich + ": a tap beside the game's screen goes nowhere");
		}
	}

	// screens that are not wide keep the 800x600 screen
	std::printf("narrow screens\n");
	Check(Compute(GameInput(1180, 820, Box{ 0, 0, 1180, 800 }, 6, true)).empty(), "iPad keeps the 800x600 screen");
	Check(Compute(GameInput(800, 600, Box{ 0, 0, 800, 600 }, 6, true)).empty(), "a 4:3 window keeps the 800x600 screen");
	Check(Compute(GameInput(1280, 800, Box{ 0, 0, 1280, 800 }, 6, true)).empty(), "a 16:10 window keeps the 800x600 screen");
	Check(ComputeFramed(GameInput(1180, 820, Box{ 0, 0, 1180, 800 }, 6, true)).empty(), "iPad keeps the 800x600 screen letterboxed outside the levels");

	// a tap outside every tile hits nothing
	std::vector<Tile> aTiles = Compute(GameInput(852, 393, aPhoneSafe, 6, true));
	double x = 0, y = 0;
	Check(!WindowToGame(aTiles, 10, 10, x, y), "a tap under the Dynamic Island hits nothing");

	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
