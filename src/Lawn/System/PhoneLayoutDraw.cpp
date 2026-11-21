/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#include "PhoneLayoutDraw.h"
#include "PhoneLayout.h"
#include "LawnApp.h"
#include "Resources.h"
#include "Lawn/Board.h"
#include "Lawn/Challenge.h"
#include "Lawn/Cutscene.h"
#include "Lawn/SeedPacket.h"
#include "Lawn/Zombie.h"
#include "Lawn/LawnMower.h"
#include "Lawn/MessageWidget.h"
#include "Lawn/System/PoolEffect.h"
#include "Lawn/Widget/GameButton.h"
#include "Lawn/Widget/AchievementsScreen.h"
#include "Lawn/Widget/AwardScreen.h"
#include "Lawn/Widget/CreditScreen.h"
#include "Lawn/Widget/GameSelector.h"
#include "Lawn/Widget/StoreScreen.h"
#include "Lawn/Widget/TitleScreen.h"
#include "Lawn/Widget/ZombatarWidget.h"
#include "PvzpLib/EffectSystem.h"
#include "PvzpLib/Reanimator.h"
#include "PvzpLib/PvzpCommon.h"
#include "graphics/Font.h"
#include "graphics/GLImage.h"
#include "graphics/MemoryImage.h"
#include "graphics/GLInterface.h"
#include "graphics/Graphics.h"
#include "misc/SexyMatrix.h"
#include "widget/WidgetManager.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <compare>
#include <map>
#include <memory>
#include <span>
#include <vector>
#include <string>

using namespace Sexy;

namespace PhoneLayoutDraw
{

namespace
{

// A piece of an image stretched into a rectangle, in the units being drawn in
void Piece(Graphics* g, Image* theImage, int theDestX, int theDestY, int theDestWidth, int theDestHeight, int theSrcX, int theSrcY, int theSrcWidth, int theSrcHeight)
{
	if (theDestWidth <= 0 || theDestHeight <= 0 || theSrcWidth <= 0 || theSrcHeight <= 0)
		return;
	g->DrawImage(theImage, Rect(theDestX, theDestY, theDestWidth, theDestHeight), Rect(theSrcX, theSrcY, theSrcWidth, theSrcHeight));
}

// A rectangle of an image drawn into a larger one in nine pieces: its corners as they are, its edges and middle stretched.
// theColumns and theRows: where the image is cut, from its left and top (four values each, the outer two its edges).
void Stretched(Graphics* g, Image* theImage, int theDestX, int theDestY, int theDestWidth, int theDestHeight, const int theColumns[4], const int theRows[4])
{
	int aDestColumns[4] = { theDestX, theDestX + theColumns[1] - theColumns[0], theDestX + theDestWidth - (theColumns[3] - theColumns[2]), theDestX + theDestWidth };
	int aDestRows[4] = { theDestY, theDestY + theRows[1] - theRows[0], theDestY + theDestHeight - (theRows[3] - theRows[2]), theDestY + theDestHeight };
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			Piece(g, theImage, aDestColumns[c], aDestRows[r], aDestColumns[c + 1] - aDestColumns[c], aDestRows[r + 1] - aDestRows[r],
				theColumns[c], theRows[r], theColumns[c + 1] - theColumns[c], theRows[r + 1] - theRows[r]);
}

// A string at a scale, its left end at x and its baseline at y
void ScaledString(Graphics* g, _Font* theFont, const std::string& theString, float x, float y, float theScale, const Color& theColor)
{
	SexyMatrix3 aMatrix;
	PvzpScaleTransformMatrix(aMatrix, x, y, theScale, theScale);
	PvzpDrawStringMatrix(g, theFont, aMatrix, theString, theColor);
}

// The card of a packet, in card units (PhoneLayout::CARD_WIDTH x CARD_HEIGHT) from 0,0: the frame and the picture of
// the packet stretched wide, the plant large on the left, the strip with the cost and its sun in the bottom right
// corner; grayed and darkened from the top as the packet is in the bank.
void DrawCard(Graphics* g, SeedPacket& thePacket)
{
	const int W = static_cast<int>(PhoneLayout::CARD_WIDTH), H = static_cast<int>(PhoneLayout::CARD_HEIGHT);
	float aPercentDark = 0;
	int aGrayness = 255;
	bool aDrawCost = true;
	thePacket.GetDrawState(aPercentDark, aGrayness, aDrawCost);
	if (aGrayness != 255)
	{
		g->SetColor(Color(aGrayness, aGrayness, aGrayness));
		g->SetColorizeImages(true);
	}
	else if (aPercentDark > 0)
	{
		g->SetColor(Color(128, 128, 128));
		g->SetColorizeImages(true);
	}

	// the packet's image (50x70): its outline and frame at the top and bottom, without the label above the picture or
	// the cost strip below it; the picture stretched
	int aCel = SeedPacketBackground(thePacket.mPacketType, thePacket.mImitaterType) * SEED_PACKET_WIDTH;
	const int aColumns[4] = { aCel, aCel + 5, aCel + 45, aCel + 50 };
	struct Band { int mSrcY, mSrcHeight, mDestY, mDestHeight; };
	const Band aBands[] = {
		{ 0, 2, 0, 2 },				// the outline and the frame along the top
		{ 7, 4, 2, 4 },				// the top of the picture's frame
		{ 11, 38, 6, H - 14 },		// the picture
		{ 49, 5, H - 8, 5 },		// the bottom of the picture's frame
		{ 67, 3, H - 3, 3 },		// the frame and the outline along the bottom
	};
	for (const Band& b : aBands)
	{
		const int aRows[4] = { b.mSrcY, b.mSrcY, b.mSrcY + b.mSrcHeight, b.mSrcY + b.mSrcHeight };
		Stretched(g, IMAGE_SEEDS, 0, b.mDestY, W, b.mDestHeight, aColumns, aRows);
	}

	// the plant, as in the packet but larger, with its middle left of the card's
	float aScale, aOffsetX, aOffsetY;
	bool aDrawPlant;
	SeedPacketSeedPlacement(thePacket.mPacketType, thePacket.mImitaterType, aScale, aOffsetX, aOffsetY, aDrawPlant);
	if (aDrawPlant)
	{
		const float k = 1.3f;
		Graphics aPlantG(*g);
		aPlantG.mScaleX = aPlantG.mScaleY = k;
		aPlantG.ClipRect(0, 0, W, H);	// (a zombie's feet or bucket would reach out of the card)
		SeedPacketDrawSeed(&aPlantG, 25 - 25 * k, 24 - 29 * k, thePacket.mPacketType, thePacket.mImitaterType, aOffsetX * k, aOffsetY * k, aScale);
	}

	// the cost, in the strip of the packet with its sun, in the bottom right corner
	if (aDrawCost)
	{
		const int aStripX = W - 47, aStripY = H - 16;
		Piece(g, IMAGE_SEEDS, aStripX, aStripY, 46, 14, aCel + 2, 54, 46, 14);
		std::string aCost = SeedPacketCostString(thePacket.mPacketType, thePacket.mImitaterType, true);
		_Font* aFont = FONT_CONTINUUMBOLD14;
		// as large as fits left of the sun (with a little room at the strip's left end), its digits in the middle of the
		// strip's height (the font draws them lower than its ascent says, and a little below the baseline it is given)
		const float aRoom = 26;
		float aTextScale = std::min(0.8f, aRoom / std::max(1, aFont->StringWidth(aCost)));
		float aDigitHeight = aFont->GetAscent() * 0.8f * aTextScale;
		ScaledString(g, aFont, aCost, aStripX + 31 - aFont->StringWidth(aCost) * aTextScale, aStripY + 7 + aDigitHeight / 2 - 2.5f * aTextScale, aTextScale,
			Color::Black);
	}
	g->SetColorizeImages(false);

	// recharging: darker from the top, as much as is left
	if (aPercentDark > 0)
	{
		int aDark = std::clamp(static_cast<int>(H * aPercentDark + 0.5f), 0, H);
		g->SetColor(Color(0, 0, 0, 128));
		g->FillRect(0, 0, W, aDark);
	}
}

// The sun counter, in units (PhoneLayout::SUN_COUNTER_WIDTH x SUN_COUNTER_HEIGHT) from 0,0: the wooden board of the
// game's sun counter, its sun on the left and its paper with the number beside it
void DrawSunCounter(Graphics* g, Board* theBoard)
{
	const int W = static_cast<int>(PhoneLayout::SUN_COUNTER_WIDTH), H = static_cast<int>(PhoneLayout::SUN_COUNTER_HEIGHT);
	Image* aBank = IMAGE_SUNBANK;	// 78x87: the board, the sun in its round frame, the paper below
	const int aColumns[4] = { 0, 8, 70, 78 };
	const int aRows[4] = { 0, 8, 79, 87 };
	Stretched(g, aBank, 0, 0, W, H, aColumns, aRows);
	Piece(g, aBank, 2, 2, H - 4, H - 4, 14, 6, 50, 50);
	// the paper (in the image x 11 to 66, y 61 to 83, with tape over its top corners), its ends as they are and its
	// middle stretched
	const int aPaperX = H, aPaperY = 5, aPaperWidth = W - H - 3, aPaperHeight = H - 10;
	const int aPaperColumns[4] = { 6, 21, 56, 70 };
	const int aPaperRows[4] = { 59, 59, 84, 84 };
	Stretched(g, aBank, aPaperX, aPaperY, aPaperWidth, aPaperHeight, aPaperColumns, aPaperRows);
	float aStretchY = aPaperHeight / 25.0f;
	float aInsideLeft = aPaperX + (11 - 6) + 4, aInsideRight = aPaperX + aPaperWidth - (70 - 66) - 7;
	float aInsideTop = aPaperY + (61 - 59) * aStretchY + 2, aInsideBottom = aPaperY + (83 - 59) * aStretchY - 2;

	// the number inside the paper: as large as fits, centred
	std::string aMoney = std::to_string(std::max(theBoard->mSunMoney, 0));
	Color aColor(0, 0, 0);
	if (theBoard->mOutOfMoneyCounter > 0 && theBoard->mOutOfMoneyCounter % 20 < 10)
		aColor = Color(255, 0, 0);
	_Font* aFont = FONT_CONTINUUMBOLD14;
	const float aDigitHeight = aFont->GetAscent() * 0.8f;	// the font's digits are lower than its ascent
	float aScale = std::min({ 1.25f, (aInsideRight - aInsideLeft) / std::max(1, aFont->StringWidth(aMoney)), (aInsideBottom - aInsideTop) / aDigitHeight });
	float aWidth = aFont->StringWidth(aMoney) * aScale;
	// (the font draws its digits a little below the baseline it is given: 2.5 units up to centre them)
	ScaledString(g, aFont, aMoney, (aInsideLeft + aInsideRight - aWidth) / 2, (aInsideTop + aInsideBottom + aDigitHeight * aScale) / 2 - 2.5f, aScale, aColor);
}

// The conveyor belt, in units along it (theLength) and across it (theWidth) from 0,0, running along x: the panel of the
// game's conveyor belt, and its belt moving inside, stretched across
constexpr int CONVEYOR_END = 8;	// units of the panel's ends
void DrawConveyor(Graphics* g, Board* theBoard, int theLength, int theWidth)
{
	Image* aPanel = IMAGE_CONVEYORBELT_BACKDROP;	// 516x86, a metal frame
	const int aColumns[4] = { 0, CONVEYOR_END, 516 - CONVEYOR_END, 516 };
	const int aRows[4] = { 0, 8, 78, 86 };
	Stretched(g, aPanel, 0, 0, theLength, theWidth, aColumns, aRows);
	Image* aBelt = IMAGE_CONVEYORBELT;	// 502x16 in 6 cels, one under another
	int aCelHeight = aBelt->GetCelHeight();
	int aCel = theBoard->mSeedBank->mConveyorBeltCounter / 4 % 6;
	const int aInset = 7, aBeltLength = aBelt->GetWidth() - 2;
	for (int x = aInset; x < theLength - aInset; x += aBeltLength)
	{
		int aLength = std::min(aBeltLength, theLength - aInset - x);
		Piece(g, aBelt, x, aInset, aLength, theWidth - 2 * aInset, 1, aCel * aCelHeight, aLength, aCelHeight);
	}
}

// Two colours mixed, theT of the second (their alphas weighing their colours, so that a clear one adds no colour)
uint32_t Mix(uint32_t theA, uint32_t theB, float theT)
{
	float aAlphaA = static_cast<float>(theA >> 24) * (1 - theT), aAlphaB = static_cast<float>(theB >> 24) * theT;
	float anAlpha = aAlphaA + aAlphaB;
	if (anAlpha <= 0)
		return 0;
	uint32_t aResult = static_cast<uint32_t>(std::lround(std::min(anAlpha, 255.0f))) << 24;
	for (int aShift = 0; aShift <= 16; aShift += 8)
	{
		float c = (static_cast<float>((theA >> aShift) & 255) * aAlphaA + static_cast<float>((theB >> aShift) & 255) * aAlphaB) / anAlpha;
		aResult |= static_cast<uint32_t>(std::lround(std::clamp(c, 0.0f, 255.0f))) << aShift;
	}
	return aResult;
}

// A picture going on past one of its edges, made of itself rather than mirrored (which shows its things twice, facing
// each other): the strip of it next to that edge laid again and again outward, each copy's far end fading into the near
// end of the next, so that they join without a seam; and where they meet the picture, its edge mirrored fading into the
// first copy within a few pixels, so that they join it without a seam either. With mFollowTop, for a layer with clear sky
// above it, each column of the strip moved up or down so that its top is level with the top of the edge's column: the
// layer goes on level from the edge, as ground does past the foot of a hill.
struct Extension
{
	std::unique_ptr<GLImage>	mRepeat;	// mPeriod wide, laid outward from the edge again and again
	std::unique_ptr<GLImage>	mJoin;		// mJoinWidth wide, over the first copy next to the edge
	int							mPeriod = 0;
	int							mJoinWidth = 0;
	int							mSrcY = 0;	// the rows: the picture's from mSrcY
};

struct ExtensionKey
{
	Image*	mImage;
	bool	mRight;
	int		mPeriod, mOverlap, mJoin, mSrcY, mSrcHeight;
	bool	mFollowTop;
	int		mInset, mStripOffset;
	bool	mCalm;
	auto	operator<=>(const ExtensionKey&) const = default;
};

// The extension of a picture past its right edge (theRight) or its left (taken theInset columns in from it, past a soft
// edge): copies thePeriod wide of its strip of thePeriod + theOverlap columns next to that edge (or theStripOffset
// columns further in, where the picture has what is to go on), each fading into the next over theOverlap columns, and
// the join theJoin wide; of its rows theSrcY to theSrcY + theSrcHeight (all below theSrcY when negative). With theCalm
// the strip without what stands out of each of its rows (the stones in earth, which would show again and again in a
// row): their pixels as the nearest others of the row. Built once; null when the picture is too small for it or its
// pixels are not at hand.
Extension* GetExtension(LawnApp* theApp, Image* theImage, bool theRight, int thePeriod, int theOverlap, int theJoin, int theSrcY = 0, int theSrcHeight = -1,
	bool theFollowTop = false, int theInset = 0, int theStripOffset = 0, bool theCalm = false)
{
	static std::map<ExtensionKey, std::unique_ptr<Extension>> gExtensions;
	ExtensionKey aKey{ theImage, theRight, thePeriod, theOverlap, theJoin, theSrcY, theSrcHeight, theFollowTop, theInset, theStripOffset, theCalm };
	auto anItr = gExtensions.find(aKey);
	if (anItr != gExtensions.end())
		return anItr->second.get();
	std::unique_ptr<Extension>& anExtension = gExtensions[aKey];

	MemoryImage* aPicture = dynamic_cast<MemoryImage*>(theImage);
	const uint32_t* aBits = aPicture ? aPicture->GetBits() : nullptr;
	if (aBits == nullptr)
		return nullptr;
	int w = aPicture->mWidth, h = aPicture->mHeight;
	int aTop = std::clamp(theSrcY, 0, h), aBottom = theSrcHeight < 0 ? h : std::clamp(theSrcY + theSrcHeight, aTop, h);
	int aHeight = aBottom - aTop;
	int aStrip = thePeriod + theOverlap;
	if (aHeight <= 0 || theOverlap <= 0 || theJoin <= 0 || theInset < 0 || theStripOffset < 0 || theInset + theStripOffset + aStrip > w ||
		theInset + theJoin > w || theJoin > thePeriod - theOverlap)
		return nullptr;

	// the picture's column d in from the edge, and the strip's column d; with theFollowTop how far down from the edge
	// column's top theirs lie (the rows of a column without a top as the one before's)
	auto EdgeColumn = [&](int d) { return theRight ? w - 1 - theInset - d : theInset + d; };
	auto TopOf = [&](int x)
	{
		for (int y = aTop; y < aBottom; y++)
			if ((aBits[y * w + x] >> 24) >= 128)
				return y;
		return -1;
	};
	int anEdgeTop = theFollowTop ? TopOf(EdgeColumn(0)) : -1;
	auto Lowered = [&](int theColumns, int theFirst)
	{
		std::vector<int> aLower(theColumns, 0);
		for (int d = 0; d < theColumns && anEdgeTop >= 0; d++)
		{
			int t = TopOf(EdgeColumn(theFirst + d));
			aLower[d] = t >= 0 ? t - anEdgeTop : d > 0 ? aLower[d - 1] : 0;
		}
		return aLower;
	};
	std::vector<int> aStripLower = Lowered(aStrip, theStripOffset), aJoinLower = Lowered(theJoin, 0);
	// a pixel of a column (theFirst + d in from the edge) at row y of the extension: below the picture its last row
	// again, above it clear
	auto Pixel = [&](int theFirst, const std::vector<int>& theLower, int d, int y) -> uint32_t
	{
		int aRow = aTop + y + theLower[d];
		if (aRow < aTop)
			return 0;
		return aBits[std::min(aRow, aBottom - 1) * w + EdgeColumn(theFirst + d)];
	};
	// the strip, d in from its first column at row y of the extension
	std::vector<uint32_t> aStripPixels(static_cast<size_t>(aStrip) * aHeight);
	for (int y = 0; y < aHeight; y++)
		for (int d = 0; d < aStrip; d++)
			aStripPixels[static_cast<size_t>(y) * aStrip + d] = Pixel(theStripOffset, aStripLower, d, y);
	if (theCalm)
	{
		// what stands out of its row (far from the row's middle colour), a little more around it, as the nearest other
		// pixel of the row
		auto Channel = [](uint32_t p, int theShift) { return static_cast<int>((p >> theShift) & 255); };
		std::vector<char> anOut(aStripPixels.size(), 0), aWider(aStripPixels.size(), 0);
		std::vector<int> aValues(aStrip);
		for (int y = 0; y < aHeight; y++)
		{
			uint32_t* aRow = &aStripPixels[static_cast<size_t>(y) * aStrip];
			int aMiddle[3];
			for (int c = 0; c < 3; c++)
			{
				for (int d = 0; d < aStrip; d++)
					aValues[d] = Channel(aRow[d], c * 8);
				std::nth_element(aValues.begin(), aValues.begin() + aStrip / 2, aValues.end());
				aMiddle[c] = aValues[aStrip / 2];
			}
			for (int d = 0; d < aStrip; d++)
			{
				int aDistance = 0;
				for (int c = 0; c < 3; c++)
					aDistance += std::abs(Channel(aRow[d], c * 8) - aMiddle[c]);
				anOut[static_cast<size_t>(y) * aStrip + d] = aDistance > 40;
			}
		}
		for (int y = 0; y < aHeight; y++)
			for (int d = 0; d < aStrip; d++)
				if (anOut[static_cast<size_t>(y) * aStrip + d])
					for (int dy = -2; dy <= 2; dy++)
						for (int dd = -3; dd <= 3; dd++)
							if (y + dy >= 0 && y + dy < aHeight && d + dd >= 0 && d + dd < aStrip)
								aWider[static_cast<size_t>(y + dy) * aStrip + d + dd] = 1;
		for (int y = 0; y < aHeight; y++)
		{
			uint32_t* aRow = &aStripPixels[static_cast<size_t>(y) * aStrip];
			const char* aMasked = &aWider[static_cast<size_t>(y) * aStrip];
			std::vector<uint32_t> aCalm(aRow, aRow + aStrip);
			for (int d = 0; d < aStrip; d++)
			{
				if (!aMasked[d])
					continue;
				for (int k = 1; k < aStrip; k++)
				{
					if (d - k >= 0 && !aMasked[d - k]) { aCalm[d] = aRow[d - k]; break; }
					if (d + k < aStrip && !aMasked[d + k]) { aCalm[d] = aRow[d + k]; break; }
				}
			}
			std::copy(aCalm.begin(), aCalm.end(), aRow);
		}
	}
	auto StripPixel = [&](int d, int y) { return aStripPixels[static_cast<size_t>(y) * aStrip + d]; };
	// outward from the edge, pixel u of a copy: the strip's columns in their order (the strip moved out by a period), its
	// last theOverlap fading into the columns a period further in (with which the next copy's first column joins)
	auto Copy = [&](int u, int y) -> uint32_t
	{
		uint32_t aColor = StripPixel(thePeriod - 1 - u, y);
		if (u >= thePeriod - theOverlap)
			aColor = Mix(aColor, StripPixel(2 * thePeriod - 1 - u, y), static_cast<float>(u - (thePeriod - theOverlap) + 1) / theOverlap);
		return aColor;
	};

	auto MakeImage = [&](int theWidth, auto theColor)
	{
		auto anImage = std::make_unique<GLImage>(theApp->mGLInterface.get());
		anImage->Create(theWidth, aHeight);
		uint32_t* anOut = anImage->GetBits();
		for (int y = 0; y < aHeight; y++)
			for (int c = 0; c < theWidth; c++)
				anOut[y * theWidth + c] = theColor(theRight ? c : theWidth - 1 - c, y);	// u outward from the edge
		anImage->mHasAlpha = true;
		anImage->mHasTrans = true;
		anImage->BitsChanged();
		return anImage;
	};
	anExtension = std::make_unique<Extension>();
	anExtension->mPeriod = thePeriod;
	anExtension->mJoinWidth = theJoin;
	anExtension->mSrcY = aTop;
	anExtension->mRepeat = MakeImage(thePeriod, Copy);
	anExtension->mJoin = MakeImage(theJoin, [&](int u, int y) -> uint32_t
	{
		float t = (u + 0.5f) / theJoin;
		return Mix(Pixel(0, aJoinLower, u, y), Copy(u, y), t * t * (3 - 2 * t));	// the edge mirrored, fading into the first copy
	});
	return anExtension.get();
}

// An extension laid past its picture's edge (at theEdgeX, the picture's top at theY, in what theToWindow takes) out to
// theFar, past the edge
void DrawExtension(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow, const Extension* theExtension, bool theRight, float theEdgeX, float theY,
	float theFar)
{
	if (theExtension == nullptr || (theRight ? theFar <= theEdgeX : theFar >= theEdgeX))
		return;
	SexyTransform2D aPlace;
	aPlace.Translate(theEdgeX, theY + theExtension->mSrcY);
	theApp->mGLInterface->PushTransform(theToWindow * aPlace, false);
	int P = theExtension->mPeriod;
	int aCopies = static_cast<int>(std::ceil(std::abs(theFar - theEdgeX) / P));
	for (int i = 0; i < aCopies; i++)
		g->DrawImage(theExtension->mRepeat.get(), theRight ? i * P : -(i + 1) * P, 0);
	g->DrawImage(theExtension->mJoin.get(), theRight ? 0 : -theExtension->mJoinWidth, 0);
	theApp->mGLInterface->PopTransform();
}

// The edge of an image (theRight: its right edge, else its left, theInset columns in from it) as a column: each row's
// colour over the columns next to that edge, softened up and down; built once for each image and edge
GLImage* GetEdgeColumn(LawnApp* theApp, Image* theImage, bool theRight, int theInset = 0)
{
	static std::map<std::tuple<Image*, bool, int>, std::unique_ptr<GLImage>> gColumns;
	auto aKey = std::make_tuple(theImage, theRight, theInset);
	auto anItr = gColumns.find(aKey);
	if (anItr != gColumns.end())
		return anItr->second.get();
	std::unique_ptr<GLImage>& aColumn = gColumns[aKey];
	MemoryImage* anImage = dynamic_cast<MemoryImage*>(theImage);
	const uint32_t* aBits = anImage ? anImage->GetBits() : nullptr;
	const int COLUMNS = 24, SOFTEN = 16, PASSES = 3;	// (softened much: a wash of the edge's colours rather than streaks)
	if (aBits == nullptr || anImage->mWidth < theInset + COLUMNS || anImage->mHeight <= 0)
		return nullptr;
	int w = anImage->mWidth, h = anImage->mHeight;
	std::vector<float> aChannels[3];
	for (int c = 0; c < 3; c++)
		aChannels[c].assign(h, 0.0f);
	for (int y = 0; y < h; y++)
		for (int i = theInset; i < theInset + COLUMNS; i++)
		{
			uint32_t p = aBits[y * w + (theRight ? w - 1 - i : i)];
			for (int c = 0; c < 3; c++)
				aChannels[c][y] += static_cast<float>((p >> (16 - 8 * c)) & 255) / COLUMNS;
		}
	for (int aPass = 0; aPass < PASSES; aPass++)
		for (int c = 0; c < 3; c++)
		{
			std::vector<float> aSoft(h);
			for (int y = 0; y < h; y++)
			{
				float aSum = 0;
				for (int k = -SOFTEN; k <= SOFTEN; k++)
					aSum += aChannels[c][std::clamp(y + k, 0, h - 1)];
				aSoft[y] = aSum / (2 * SOFTEN + 1);
			}
			aChannels[c] = std::move(aSoft);
		}
	aColumn = std::make_unique<GLImage>(theApp->mGLInterface.get());
	aColumn->Create(1, h);
	uint32_t* anOut = aColumn->GetBits();
	for (int y = 0; y < h; y++)
	{
		uint32_t p = 0xFF000000u;
		for (int c = 0; c < 3; c++)
			p |= static_cast<uint32_t>(std::lround(std::clamp(aChannels[c][y], 0.0f, 255.0f))) << (16 - 8 * c);
		anOut[y] = p;
	}
	aColumn->BitsChanged();
	return aColumn.get();
}

// From an end of an image (at theFromX, in window points) out to theToX: the image's edge going on (its edge column,
// stretched; theTop and theHeight where the image's rows are), darkening further out, as a shadow there; and over its
// first few points the image itself mirrored there (theImage, theScale window points to its pixels, its edge column
// theEdgeColumn), fading into it, so that the image does not end in a line
void DrawEdgeFill(Graphics* g, GLImage* theColumn, double theFromX, double theToX, double theTop, double theHeight, Image* theImage = nullptr,
	double theScale = 1, int theEdgeColumn = 0, float theDarkest = 0.72f)
{
	if (theColumn == nullptr || std::abs(theToX - theFromX) < 0.01)
		return;
	int aLeft = static_cast<int>(std::floor(std::min(theFromX, theToX))), aRight = static_cast<int>(std::ceil(std::max(theFromX, theToX)));
	int aTop = static_cast<int>(std::floor(theTop)), aBottom = static_cast<int>(std::ceil(theTop + theHeight));
	g->DrawImage(theColumn, Rect(aLeft, aTop, aRight - aLeft, aBottom - aTop), Rect(0, 0, 1, theColumn->mHeight));
	bool anOutRight = theToX > theFromX;
	if (theImage)
	{
		// the image's columns next to its end, mirrored out from it, fading
		const int JOIN = 20;
		g->SetColorizeImages(true);
		for (int i = 0; i < JOIN; i++)
		{
			int aColumn = anOutRight ? theEdgeColumn - i : theEdgeColumn + i;
			if (aColumn < 0 || aColumn >= theImage->mWidth)
				break;
			double x = anOutRight ? theFromX + i * theScale : theFromX - (i + 1) * theScale;
			float t = (i + 0.5f) / JOIN;
			g->SetColor(Color(255, 255, 255, static_cast<int>(std::lround(255 * (1 - t * t * (3 - 2 * t))))));
			int aX = static_cast<int>(std::floor(x)), aWidth = std::max(1, static_cast<int>(std::ceil(x + theScale)) - aX);
			g->DrawImage(theImage, Rect(aX, aTop, aWidth, aBottom - aTop), Rect(aColumn, 0, 1, theImage->mHeight));
		}
		g->SetColorizeImages(false);
	}
	const float DISTANCE = 100;	// how far out from the image's end it gets as dark as theDarkest
	for (int x = aLeft; x < aRight; x++)
	{
		float t = std::clamp(static_cast<float>(std::abs(x + 0.5 - theFromX)) / DISTANCE, 0.0f, 1.0f);
		int anAlpha = static_cast<int>(std::lround(255 * theDarkest * t * t * (3 - 2 * t)));
		if (anAlpha <= 0)
			continue;
		g->SetColor(Color(0, 0, 0, anAlpha));
		g->FillRect(x, aTop, 1, aBottom - aTop);
	}
}

// A strip of a picture (theStripX to theStripX + theStripWidth of it, all its height) going on left of where the picture
// lies (theX, theY in what theToWindow takes), again and again to theLeft, every other copy mirrored so that they join
// without a seam (the one next to the picture mirrored)
void DrawStripsLeft(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow, Image* theImage, int theStripX, int theStripWidth, float theX, float theY,
	float theLeft)
{
	for (int i = 0; theX - i * theStripWidth > theLeft; i++)
	{
		// the strip's x from theStripX to [theX - (i + 1) w, theX - i w]
		bool aMirrored = i % 2 == 0;
		SexyMatrix3 aPlace;
		aPlace.m00 = aMirrored ? -1.0f : 1.0f; aPlace.m01 = 0;
		aPlace.m02 = aMirrored ? theX - i * theStripWidth + theStripX : theX - (i + 1) * theStripWidth - theStripX;
		aPlace.m10 = 0; aPlace.m11 = 1; aPlace.m12 = theY;
		aPlace.m20 = 0; aPlace.m21 = 0; aPlace.m22 = 1;
		theApp->mGLInterface->PushTransform(theToWindow * aPlace, false);
		g->DrawImage(theImage, theStripX, 0, Rect(theStripX, 0, theStripWidth, theImage->mHeight));
		theApp->mGLInterface->PopTransform();
	}
}

// The right edge of the main menu's tombstone, which its picture (the tombstone with the ground under it) cuts: where
// the picture's last column is stone, a few pixels more of the stone's face beyond it (bulging to the middle of the cut)
// and its dark outline, as the rest of the tombstone has; built once from the picture
GLImage* GetStoneEdge(LawnApp* theApp, Image* thePicture)
{
	static Image* gPicture = nullptr;
	static std::unique_ptr<GLImage> gEdge;
	if (gPicture == thePicture)
		return gEdge.get();
	gPicture = thePicture;
	gEdge.reset();
	MemoryImage* aPicture = dynamic_cast<MemoryImage*>(thePicture);
	const uint32_t* aBits = aPicture ? aPicture->GetBits() : nullptr;
	const int TOP = 40, BOTTOM = 300, WIDTH = 8;	// the stone's part of the picture (the ground below)
	if (aBits == nullptr || aPicture->mWidth < 8 || aPicture->mHeight < BOTTOM)
		return nullptr;
	int w = aPicture->mWidth;
	auto IsStone = [&](int y) { return (aBits[y * w + w - 1] >> 24) >= 128; };
	int aFirst = TOP, aLast = BOTTOM - 1;
	while (aFirst < BOTTOM && !IsStone(aFirst))
		aFirst++;
	while (aLast > aFirst && !IsStone(aLast))
		aLast--;
	if (aLast - aFirst < 8)
		return nullptr;
	const uint32_t anOutline = 0xFF0A0C18u;	// the dark blue of the tombstone's outline
	gEdge = std::make_unique<GLImage>(theApp->mGLInterface.get());
	gEdge->Create(WIDTH, aPicture->mHeight);
	uint32_t* anOut = gEdge->GetBits();
	std::fill(anOut, anOut + WIDTH * aPicture->mHeight, 0u);
	for (int y = aFirst; y <= aLast; y++)
	{
		float t = static_cast<float>(y - aFirst) / (aLast - aFirst);
		float anEnd = 0.5f + 3.5f * std::sin(t * PI);	// the outline's outer edge, pixels past the picture's
		uint32_t aFace = aBits[y * w + w - 3] | 0xFF000000u;
		for (int x = 0; x < WIDTH; x++)
		{
			float aCover = std::clamp(anEnd - x, 0.0f, 1.0f);
			if (aCover <= 0)
				break;
			uint32_t aColor = x + 2.0f < anEnd ? aFace : anOutline;
			anOut[y * WIDTH + x] = (aColor & 0x00FFFFFFu) | (static_cast<uint32_t>(std::lround(aCover * 255)) << 24);
		}
	}
	gEdge->mHasAlpha = true;
	gEdge->mHasTrans = true;
	gEdge->BitsChanged();
	return gEdge.get();
}

// The sky of Zombatar's picture, the moon taken out of it (its bright, grey pixels as the dark sky of their row; the
// clouds in front of it mostly kept): to make its sky go on with, without a second moon; built once
Image* GetMoonlessZombatarSky(LawnApp* theApp)
{
	static Image* gPicture = nullptr;
	static std::unique_ptr<GLImage> gSky;
	if (gPicture == IMAGE_ZOMBATAR_MAIN_BG && gSky)
		return gSky.get();
	gPicture = IMAGE_ZOMBATAR_MAIN_BG;
	gSky.reset();
	MemoryImage* aPicture = dynamic_cast<MemoryImage*>(gPicture);
	const uint32_t* aBits = aPicture ? aPicture->GetBits() : nullptr;
	if (aBits == nullptr)
		return gPicture;
	int w = aPicture->mWidth, h = aPicture->mHeight;
	gSky = std::make_unique<GLImage>(theApp->mGLInterface.get());
	gSky->Create(w, h);
	uint32_t* anOut = gSky->GetBits();
	std::copy(aBits, aBits + static_cast<size_t>(w) * h, anOut);
	auto Channel = [](uint32_t p, int theShift) { return static_cast<float>((p >> theShift) & 255); };
	auto Luma = [&](uint32_t p) { return 0.3f * Channel(p, 16) + 0.59f * Channel(p, 8) + 0.11f * Channel(p, 0); };
	const float MOON_X = 680, MOON_Y = 78, MOON_RADIUS = 80, MOON_REACH = 115;	// the moon, its disc and its glow
	const int FROM = 540, SKY_BOTTOM = 215;
	std::vector<std::pair<float, uint32_t>> aRow;
	for (int y = 0; y < std::min(h, SKY_BOTTOM); y++)
	{
		// the row's sky: the average of its darkest pixels right of FROM (the clouds, the moon and the stars are brighter)
		aRow.clear();
		for (int x = FROM; x < w; x++)
			aRow.emplace_back(Luma(aBits[y * w + x]), aBits[y * w + x]);
		std::sort(aRow.begin(), aRow.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		size_t aDarkest = std::max<size_t>(1, aRow.size() / 6);
		float aSky[3] = { 0, 0, 0 };
		for (size_t i = 0; i < aDarkest; i++)
			for (int c = 0; c < 3; c++)
				aSky[c] += Channel(aRow[i].second, 16 - 8 * c) / aDarkest;
		uint32_t aSkyColor = 0xFF000000u | (static_cast<uint32_t>(aSky[0]) << 16) | (static_cast<uint32_t>(aSky[1]) << 8) | static_cast<uint32_t>(aSky[2]);
		float aSkyLuma = Luma(aSkyColor);
		for (int x = FROM; x < w; x++)
		{
			float aDistance = std::hypot(x - MOON_X, y - MOON_Y);
			if (aDistance > MOON_REACH)
				continue;
			uint32_t p = aBits[y * w + x];
			float aGreyness = std::max({ Channel(p, 16), Channel(p, 8), Channel(p, 0) }) - std::min({ Channel(p, 16), Channel(p, 8), Channel(p, 0) });
			float aReach = std::clamp((MOON_REACH - aDistance) / 20, 0.0f, 1.0f);
			float aMoon = std::clamp((Luma(p) - 120) / 70, 0.0f, 1.0f) * (aGreyness < 26 ? 1.0f : 0.3f) * aReach;
			// the moon's faint outer glow, round its disc: what is a little lighter than the sky there (the clouds are
			// lighter than that, or green)
			bool aGreen = Channel(p, 8) - std::max(Channel(p, 16), Channel(p, 0)) > 18;
			if (!aGreen && aDistance > MOON_RADIUS - 10 && Luma(p) < 135)
				aMoon = std::max(aMoon, std::clamp((Luma(p) - aSkyLuma - 4) / 25, 0.0f, 1.0f) * aReach);
			anOut[y * w + x] = Mix(p, aSkyColor, aMoon);
		}
	}
	gSky->BitsChanged();
	return gSky.get();
}

// The right end of the mound of earth under the main menu's tombstone, which its picture (the tombstone with the ground
// under it) cuts: the mound's left end mirrored, sloping down to the meadow, as high where it meets the picture's edge
// as the ground is there (stretched up from the picture's bottom, where both stand), its first columns fading from the
// picture's edge mirrored (its grass and the stone's foot) into it; built once from the picture
GLImage* GetGroundEnd(LawnApp* theApp, Image* thePicture)
{
	static Image* gPicture = nullptr;
	static std::unique_ptr<GLImage> gEnd;
	if (gPicture == thePicture)
		return gEnd.get();
	gPicture = thePicture;
	gEnd.reset();
	MemoryImage* aPicture = dynamic_cast<MemoryImage*>(thePicture);
	const uint32_t* aBits = aPicture ? aPicture->GetBits() : nullptr;
	const int GROUND_TOP = 300, TIP = 250, JOIN = 40, INSET = 1;	// the mound's left end: the picture's columns from 0 to TIP
	if (aBits == nullptr || aPicture->mWidth < TIP + JOIN + INSET || aPicture->mHeight <= GROUND_TOP)
		return nullptr;
	int w = aPicture->mWidth, h = aPicture->mHeight;
	auto TopOf = [&](int x)
	{
		for (int y = GROUND_TOP; y < h; y++)
			if ((aBits[y * w + x] >> 24) >= 128)
				return y;
		return h;
	};
	int anEdgeTop = TopOf(w - 1 - INSET), aTipTop = TopOf(TIP - 1);
	if (anEdgeTop >= h - 8 || aTipTop >= h - 8)
		return nullptr;
	// a row of the end from a row of the mound's left end: stretched up from the bottom so that its top at TIP meets
	// the edge's
	float aStretch = static_cast<float>(h - anEdgeTop) / (h - aTipTop);
	auto Pixel = [&](int x, int y) -> uint32_t
	{
		float aSourceY = h - (h - y) / aStretch;
		int aRow = static_cast<int>(std::floor(aSourceY));
		if (aRow < 0 || aRow >= h)
			return 0;
		uint32_t a = aBits[aRow * w + x], b = aBits[std::min(aRow + 1, h - 1) * w + x];
		return Mix(a, b, aSourceY - aRow);
	};
	// the left end lit more than the right (in the shade of the tombstone): its colours brought to the right end's, as
	// their earth is on average (above the stone the tombstone stands on)
	const int SAMPLE = 20, STONE_TOP = 470;
	double aSums[2][3] = {};
	int aCounts[2] = {};
	for (int y = anEdgeTop + 10; y < std::min(h, STONE_TOP); y++)
		for (int i = 0; i < SAMPLE; i++)
		{
			uint32_t aPixels[2] = { aBits[y * w + w - 1 - INSET - i], Pixel(TIP - 1 - i, y) };
			for (int k = 0; k < 2; k++)
			{
				if ((aPixels[k] >> 24) < 200)
					continue;
				aCounts[k]++;
				for (int c = 0; c < 3; c++)
					aSums[k][c] += (aPixels[k] >> (16 - 8 * c)) & 255;
			}
		}
	float aGain[3] = { 1, 1, 1 };
	for (int c = 0; c < 3 && aCounts[0] > 0 && aCounts[1] > 0; c++)
		aGain[c] = static_cast<float>(std::clamp((aSums[0][c] / aCounts[0]) / std::max(1.0, aSums[1][c] / aCounts[1]), 0.5, 1.5));
	auto Shaded = [&](uint32_t p)
	{
		uint32_t aResult = p & 0xFF000000u;
		for (int c = 0; c < 3; c++)
			aResult |= static_cast<uint32_t>(std::clamp(std::lround(((p >> (16 - 8 * c)) & 255) * aGain[c]), 0L, 255L)) << (16 - 8 * c);
		return aResult;
	};
	gEnd = std::make_unique<GLImage>(theApp->mGLInterface.get());
	gEnd->Create(TIP, h);
	uint32_t* anOut = gEnd->GetBits();
	for (int y = 0; y < h; y++)
	{
		for (int u = 0; u < TIP; u++)
		{
			uint32_t aColor = Shaded(Pixel(TIP - 1 - u, y));	// the left end mirrored: its tip farthest
			if (u < JOIN)
			{
				float t = (u + 0.5f) / JOIN;
				uint32_t anEdge = y >= GROUND_TOP ? aBits[y * w + w - 1 - INSET - u] : 0;
				aColor = Mix(anEdge, aColor, t * t * (3 - 2 * t));
			}
			anOut[y * TIP + u] = aColor;
		}
	}
	gEnd->mHasAlpha = true;
	gEnd->mHasTrans = true;
	gEnd->BitsChanged();
	return gEnd.get();
}

// The main menu's scenery around its screen, in the game's coordinates (the transform puts them in the window): its
// sky mirrored at the edges of the screen (a smooth gradient), its clouds where they are, drifting past the edges; the
// tree mirrored on the left (its picture ends at the screen's edge), and on the right the hills going on, the tombstone
// given the right edge its picture cuts off and the mound under it ending as it does on the left. Past the edges of the
// achievements and of Zombatar, when it slides to them, their earth and graveyard going on. Around the main menu, and
// around pages (the almanac, the lists of mini-games) the same.
void DrawMenuScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindowStill)
{
	// where the main menu is now: it slides to the achievements below it and to Zombatar right of it
	GameSelector* aSlider = theApp->mGameSelector.get();
	SexyTransform2D aSlide;
	if (aSlider)
		aSlide.Translate(static_cast<float>(aSlider->mX), static_cast<float>(aSlider->mY));
	SexyMatrix3 theToWindow = theToWindowStill * aSlide;
	// the main menu's own, or without it (the lists of mini-games replace it) one of ours for this drawing, still, at the
	// end of its opening (and gone after it: a level draws every animation there is)
	GameSelector* aSelector = theApp->mGameSelector.get();
	Reanimation* aReanim = aSelector ? theApp->ReanimationTryToGet(aSelector->mSelectorReanimID) : nullptr;
	Reanimation* anOwn = nullptr;
	if (aReanim == nullptr)
	{
		anOwn = aReanim = theApp->AddReanimation(0.5f, 0.5f, 0, ReanimationType::REANIM_SELECTOR_SCREEN);
		aReanim->PlayReanim("anim_open", ReanimLoopType::REANIM_PLAY_ONCE_AND_HOLD, 0, 0.0f);
		aReanim->mAnimTime = 1.0f;
		aReanim->mAnimRate = 0.0f;
	}
	// in the game's order: the sky, the clouds, the layers in front of them (left the tree mirrored, right the hills
	// going on, below)
	const char* const SKY[] = { "SelectorScreen_BG" };
	const char* const LEFT[] = { "SelectorScreen_BG_Left" };
	const char* const RIGHT[] = { "SelectorScreen_BG_Center" };	// also mirrored left, under the tree
	auto DrawMirrored = [&](std::span<const char* const> theLeftTracks, std::span<const char* const> theRightTracks)
	{
		for (int aSide = 0; aSide < 2; aSide++)
		{
			// mirrored at x 0 or at x BOARD_WIDTH, half a pixel in (the pictures' outermost columns are a little clear)
			SexyMatrix3 aMirror;
			aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = aSide == 0 ? 1.0f : 2.0f * BOARD_WIDTH - 1.0f;
			aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = 0;
			aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
			theApp->mGLInterface->PushTransform(theToWindow * aMirror, false);
			PvzpTriangleGroup aTriangles;
			for (const char* aTrack : aSide == 0 ? theLeftTracks : theRightTracks)
			{
				int anIndex = aReanim->FindTrackIndex(aTrack);
				if (anIndex >= 0)
					aReanim->DrawTrack(g, anIndex, RENDER_GROUP_NORMAL, &aTriangles);
			}
			aTriangles.DrawGroup(g);
			theApp->mGLInterface->PopTransform();
		}
	};
	DrawMirrored(SKY, SKY);
	if (aSelector)
	{
		theApp->mGLInterface->PushTransform(theToWindow, false);
		for (ReanimationID anID : aSelector->mCloudReanimID)
			if (Reanimation* aCloud = theApp->ReanimationTryToGet(anID))
				aCloud->Draw(g);
		theApp->mGLInterface->PopTransform();
	}
	DrawMirrored(LEFT, std::span<const char* const>());

	// right of the screen a meadow going on from the hills (their last column, where they meet the ground), its horizon
	// at the top of the ground under the tombstone, which fades into it
	const int GROUND_TOP = 333;			// of the ground's picture (SelectorScreen_BG_Right), below the tombstone
	int aHillIndex = aReanim->FindTrackIndex("SelectorScreen_BG_Center");
	int aGroundIndex = aReanim->FindTrackIndex("SelectorScreen_BG_Right");
	ReanimatorTransform aGroundTransform;
	if (aGroundIndex >= 0)
		aReanim->GetCurrentTransform(aGroundIndex, &aGroundTransform);
	if (aHillIndex >= 0 && aGroundIndex >= 0)
	{
		ReanimatorTransform aTransform;
		aReanim->GetCurrentTransform(aHillIndex, &aTransform);
		MemoryImage* aHills = dynamic_cast<MemoryImage*>(aTransform.mImage);
		if (aHills && aHills->mWidth > 1 && aHills->GetBits())
		{
			// the hills' last column: clear down to where the grass begins
			const uint32_t* aBits = aHills->GetBits();
			int aGrassRow = 0;
			while (aGrassRow < aHills->mHeight - 1 && (aBits[aGrassRow * aHills->mWidth + aHills->mWidth - 1] >> 24) < 128)
				aGrassRow++;
			float aHorizon = aGroundTransform.mTransY + GROUND_TOP + 12;
			theApp->mGLInterface->PushTransform(theToWindow, false);
			g->DrawImage(aHills, Rect(BOARD_WIDTH - 1, static_cast<int>(aHorizon), 4 * BOARD_WIDTH, static_cast<int>(BOARD_HEIGHT - aHorizon) + 2),
				Rect(aHills->mWidth - 1, aGrassRow, 1, aHills->mHeight - aGrassRow));
			theApp->mGLInterface->PopTransform();
			// and over it the hills mirrored at the screen's edge, raised so that they rise from that horizon
			float aRaise = aTransform.mTransY + aGrassRow - aHorizon;
			SexyMatrix3 aMirror;
			aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = 2.0f * (aTransform.mTransX + aHills->mWidth);
			aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = -aRaise;
			aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
			theApp->mGLInterface->PushTransform(theToWindow * aMirror, false);
			g->DrawImage(aHills, static_cast<int>(aTransform.mTransX), static_cast<int>(aTransform.mTransY));
			theApp->mGLInterface->PopTransform();
		}
	}

	// right of the screen, where the picture of the tombstone and the ground under it ends (at the screen's edge, which
	// cuts them): the tombstone's right edge, and the mound of earth it stands on ending as it does on the left
	if (aGroundIndex >= 0 && aGroundTransform.mImage)
	{
		Image* aGround = aGroundTransform.mImage;
		float anEdge = aGroundTransform.mTransX + aGround->mWidth, aTop = aGroundTransform.mTransY;
		// (the layers where they are, for what of their pictures lies past the screen's edge)
		theApp->mGLInterface->PushTransform(theToWindow, false);
		{
			PvzpTriangleGroup aTriangles;
			for (const char* aTrack : { "SelectorScreen_BG_Center", "SelectorScreen_BG_Left", "SelectorScreen_BG_Right" })
			{
				int anIndex = aReanim->FindTrackIndex(aTrack);
				if (anIndex >= 0)
					aReanim->DrawTrack(g, anIndex, RENDER_GROUP_NORMAL, &aTriangles);
			}
			aTriangles.DrawGroup(g);
		}
		theApp->mGLInterface->PopTransform();
		SexyTransform2D anAtEdge;
		anAtEdge.Translate(anEdge, aTop);
		theApp->mGLInterface->PushTransform(theToWindow * anAtEdge, false);
		if (GLImage* anEnd = GetGroundEnd(theApp, aGround))
			g->DrawImage(anEnd, 0, 0);
		if (GLImage* aStoneEdge = GetStoneEdge(theApp, aGround))
			g->DrawImage(aStoneEdge, 0, 0);
		theApp->mGLInterface->PopTransform();
	}
	if (anOwn)
		anOwn->ReanimationDie();

	// the achievements below the main menu: their earth going on at both sides (made of the strips at their edges, left of
	// their title's stones and right of the bushes)
	float aLeftmost = -2.0f * BOARD_WIDTH, aRightmost = 3.0f * BOARD_WIDTH;
	if (aSelector && aSelector->mAchievementsWidget && aSelector->mAchievementsWidget->mY < BOARD_HEIGHT && IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG)
	{
		const AchievementsWidget& anAchievements = *aSelector->mAchievementsWidget;
		// (their outermost columns a little clear; the earth without its stones, which would show again and again)
		const int PERIOD = 66, OVERLAP = 22, JOIN = 14, INSET = 1;
		float x = static_cast<float>(anAchievements.mX), y = static_cast<float>(anAchievements.mY);
		int aHeight = IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG->mHeight;
		auto DrawBothSides = [&](Image* theImage, float theY, int theEarthTop)
		{
			for (int aPart = 0; aPart < 2; aPart++)
			{
				int aTop = aPart == 0 ? 0 : theEarthTop, aRows = aPart == 0 ? theEarthTop : -1;
				if (aPart == 0 && theEarthTop <= 0)
					continue;
				bool aCalm = aPart == 1;
				DrawExtension(g, theApp, theToWindowStill, GetExtension(theApp, theImage, false, PERIOD, OVERLAP, JOIN, aTop, aRows, false, INSET, 0, aCalm), false,
					x + INSET, theY, aLeftmost);
				DrawExtension(g, theApp, theToWindowStill, GetExtension(theApp, theImage, true, PERIOD, OVERLAP, JOIN, aTop, aRows, false, INSET, 0, aCalm), true,
					x + theImage->mWidth - INSET, theY, aRightmost);
			}
		};
		const int EARTH_TOP = 180;	// of the title's picture: its earth below the grass
		if (y + aHeight > 0)
			DrawBothSides(IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG, y, EARTH_TOP);
		if (IMAGE_ACHEESEMENTS_HOLE_TILE)
		{
			int aTile = IMAGE_ACHEESEMENTS_HOLE_TILE->mHeight;
			for (int i = 1; i <= 70; i++)
			{
				float aTileY = y + static_cast<float>(aHeight * i);
				if (aTileY > BOARD_HEIGHT)
					break;
				if (aTileY + aTile > 0)
					DrawBothSides(IMAGE_ACHEESEMENTS_HOLE_TILE, aTileY, 0);
			}
		}
	}

	// Zombatar right of the main menu: its graveyard going on left of it (its left part, left of its BACK stone: the
	// obelisk at its edge, its wall, the mound and the grass, again and again) and right of it
	if (aSelector && aSelector->mZombatarWidget && aSelector->mZombatarWidget->mX < BOARD_WIDTH && IMAGE_ZOMBATAR_MAIN_BG)
	{
		const ZombatarWidget& aZombatar = *aSelector->mZombatarWidget;
		DrawStripsLeft(g, theApp, theToWindowStill, IMAGE_ZOMBATAR_MAIN_BG, 0, 280, static_cast<float>(aZombatar.mX), static_cast<float>(aZombatar.mY), aLeftmost);
		// right of it its sky and clouds going on (made of the sky round the moon, the moon taken out of it: it stays one),
		// and its wall and the grass (level, plain stones: mirrored at the edge, its raised end and then its lower part)
		const int SKY_BOTTOM = 215, WALL_STRIP = 260;
		float anEdge = static_cast<float>(aZombatar.mX + IMAGE_ZOMBATAR_MAIN_BG->mWidth), aTop = static_cast<float>(aZombatar.mY);
		SexyMatrix3 aMirror;
		aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = 2 * anEdge - 1;
		aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = aTop;
		aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
		theApp->mGLInterface->PushTransform(theToWindowStill * aMirror, false);
		int aWidth = IMAGE_ZOMBATAR_MAIN_BG->mWidth, aHeight = IMAGE_ZOMBATAR_MAIN_BG->mHeight;
		g->DrawImage(IMAGE_ZOMBATAR_MAIN_BG, static_cast<int>(aZombatar.mX) + aWidth - WALL_STRIP, SKY_BOTTOM,
			Rect(aWidth - WALL_STRIP, SKY_BOTTOM, WALL_STRIP, aHeight - SKY_BOTTOM));
		theApp->mGLInterface->PopTransform();
		DrawExtension(g, theApp, theToWindowStill, GetExtension(theApp, GetMoonlessZombatarSky(theApp), true, 200, 40, 16, 0, SKY_BOTTOM), true, anEdge, aTop,
			aRightmost);
	}
}

// A page (the almanac, the lists of mini-games, an award) laid over the main menu's scenery, still, as it is when the
// main menu has just opened
void DrawPageScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	GameSelector* aSelector = theApp->mGameSelector.get();
	bool aSlid = aSelector && (aSelector->mX != 0 || aSelector->mY != 0);
	if (aSlid)
	{
		// the main menu slid away: its scenery where it was
		int x = aSelector->mX, y = aSelector->mY;
		aSelector->mX = aSelector->mY = 0;
		DrawMenuScene(g, theApp, theToWindow);
		aSelector->mX = x;
		aSelector->mY = y;
	}
	else
		DrawMenuScene(g, theApp, theToWindow);
}

// The Tree of Wisdom's sky and hill going on past the screen's edges: its sky (a smooth gradient) as its edges are, its
// clouds where they are, its hill going on level from its feet (rather than mirrored: a valley of two hills)
void DrawTreeScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	Board* aBoard = theApp->mBoard;
	if (aBoard == nullptr || !aBoard->mChallenge)
		return;
	Reanimation* aTree = theApp->ReanimationTryToGet(aBoard->mChallenge->mReanimChallenge);
	if (aTree == nullptr)
		return;
	SexyTransform2D aFromBoard;
	aFromBoard.Translate(static_cast<float>(aBoard->mX), static_cast<float>(aBoard->mY));
	SexyMatrix3 aToWindow = theToWindow * aFromBoard;
	const float REACH = 2.0f * BOARD_WIDTH;
	const int INSET = 3;	// past the pictures' outermost columns, which are a little different
	auto Track = [&](const char* theName, ReanimatorTransform& theTransform)
	{
		int anIndex = aTree->FindTrackIndex(theName);
		if (anIndex < 0)
			return static_cast<Image*>(nullptr);
		aTree->GetCurrentTransform(anIndex, &theTransform);
		return theTransform.mImage;
	};
	ReanimatorTransform aSky;
	if (Image* anImage = Track("bg", aSky))
	{
		int x = static_cast<int>(aSky.mTransX), y = static_cast<int>(aSky.mTransY);
		theApp->mGLInterface->PushTransform(aToWindow, false);
		g->DrawImage(anImage, Rect(static_cast<int>(-REACH), y, static_cast<int>(REACH) + x + INSET, anImage->mHeight), Rect(INSET, 0, 1, anImage->mHeight));
		g->DrawImage(anImage, Rect(x + anImage->mWidth - INSET - 1, y, static_cast<int>(REACH), anImage->mHeight), Rect(anImage->mWidth - INSET - 1, 0, 1, anImage->mHeight));
		theApp->mGLInterface->PopTransform();
	}
	theApp->mGLInterface->PushTransform(aToWindow, false);
	for (ReanimationID anID : aBoard->mChallenge->mReanimClouds)
	{
		if (Reanimation* aCloud = theApp->ReanimationTryToGet(anID))
		{
			Graphics aCloudG(*g);
			aCloud->DrawRenderGroup(&aCloudG, 0);
		}
	}
	theApp->mGLInterface->PopTransform();
	ReanimatorTransform aHill;
	if (Image* anImage = Track("grass", aHill))
	{
		const int PERIOD = 36, OVERLAP = 12, JOIN = 8;
		DrawExtension(g, theApp, aToWindow, GetExtension(theApp, anImage, false, PERIOD, OVERLAP, JOIN, 0, -1, true, INSET), false, aHill.mTransX + INSET,
			aHill.mTransY, -REACH);
		DrawExtension(g, theApp, aToWindow, GetExtension(theApp, anImage, true, PERIOD, OVERLAP, JOIN, 0, -1, true, INSET), true,
			aHill.mTransX + anImage->mWidth - INSET, aHill.mTransY, REACH);
	}
}

// The credits as they draw themselves, past the screen's edges (their scenes are larger than the screen; the pool's
// water cut where a level's screen cuts it), and where their picture of the house ends short of the window's edges
// (theLeft and theRight, in window points), its edge going on, darkening; the screen covers what is on it
void DrawCreditsScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow, double theLeft, double theRight)
{
	CreditScreen* aCredits = theApp->mCreditScreen.get();
	if (aCredits == nullptr)
		return;
	SexyTransform2D aPlace;
	aPlace.Translate(static_cast<float>(aCredits->mX), static_cast<float>(aCredits->mY));
	SexyMatrix3 aToWindow = theToWindow * aPlace;
	theApp->mGLInterface->PushTransform(aToWindow, false);
	{
		Graphics aCreditsG(*g);
		theApp->mDrawingPhoneOverlay = true;
		aCredits->Draw(&aCreditsG);
		theApp->mDrawingPhoneOverlay = false;
	}
	theApp->mGLInterface->PopTransform();
	Reanimation* aReanim = theApp->ReanimationTryToGet(aCredits->mCreditsReanimID);
	int anIndex = aReanim ? aReanim->FindTrackIndex("Background2") : -1;
	if (anIndex < 0)
		return;
	ReanimatorTransform aTransform;
	aReanim->GetCurrentTransform(anIndex, &aTransform);
	Image* anImage = aTransform.mImage;
	if (aTransform.mFrame == -1.0f || anImage == nullptr)
		return;
	SexyVector2 aTopLeft = aToWindow * SexyVector2(aTransform.mTransX, aTransform.mTransY);
	SexyVector2 aBottomRight = aToWindow * SexyVector2(aTransform.mTransX + anImage->mWidth * aTransform.mScaleX, aTransform.mTransY + anImage->mHeight * aTransform.mScaleY);
	double aScale = (aBottomRight.x - aTopLeft.x) / anImage->mWidth;
	if (aTopLeft.x > theLeft)
		DrawEdgeFill(g, GetEdgeColumn(theApp, anImage, false), aTopLeft.x, theLeft, aTopLeft.y, aBottomRight.y - aTopLeft.y, anImage, aScale, 0);
	if (aBottomRight.x < theRight)
		DrawEdgeFill(g, GetEdgeColumn(theApp, anImage, true), aBottomRight.x, theRight, aTopLeft.y, aBottomRight.y - aTopLeft.y, anImage, aScale, anImage->mWidth - 1);
}

// A zombie's note: the picture behind it, which the award screen draws larger than its screen, drawn again around it
void DrawNoteScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	AwardScreen* anAward = theApp->mAwardScreen.get();
	if (anAward == nullptr)
		return;
	SexyTransform2D aPlace;
	aPlace.Translate(static_cast<float>(anAward->mX), static_cast<float>(anAward->mY));
	theApp->mGLInterface->PushTransform(theToWindow * aPlace, false);
	if (anAward->mAwardType == AwardType::AWARD_CREDITS_ZOMBIENOTE)
	{
		g->SetColor(Color(125, 200, 255, 255));
		g->SetColorizeImages(true);
		g->DrawImage(IMAGE_BACKGROUND6BOSS, -900, -400, 2800, 1200);
		g->SetColorizeImages(false);
		g->SetColor(Color(0, 0, 0, 64));
		g->FillRect(-2000, 525, 4800, BOARD_HEIGHT - 525);
	}
	else
		g->DrawImage(IMAGE_BACKGROUND1, -700, -300, 2800, 1200);
	theApp->mGLInterface->PopTransform();
}

// The transform from the game's screen (theTile.mSource of it) to the window (theTile.mDest)
SexyTransform2D ToWindow(const PresentationTile& theTile)
{
	float aScaleX = static_cast<float>(theTile.mDest.mWidth / theTile.mSource.mWidth), aScaleY = static_cast<float>(theTile.mDest.mHeight / theTile.mSource.mHeight);
	SexyTransform2D aTransform;
	aTransform.Scale(aScaleX, aScaleY);
	aTransform.Translate(static_cast<float>(theTile.mDest.mX - theTile.mSource.mX * aScaleX), static_cast<float>(theTile.mDest.mY - theTile.mSource.mY * aScaleY));
	return aTransform;
}

// What the left of the store is made of, from its background (the day's or the night's, which are drawn alike): the
// column of its sky that is cleanest (no cloud, no star) to stretch, and the natural edge of its tree's crown (on the
// tree's right, where it meets the sky) cut out, its sky and clouds made clear (found in the day's picture, where leaves
// and sky part by their colours)
struct StoreArt
{
	MemoryImage*				mSource = nullptr;
	int							mSkyColumn = 0;
	std::unique_ptr<GLImage>	mCrownEdge;
};

constexpr int CROWN_EDGE_X = 240, CROWN_EDGE_WIDTH = 100, CROWN_EDGE_HEIGHT = 140, STORE_SKY_HEIGHT = 145;
constexpr int STORE_PERIOD = 150, STORE_OVERLAP = 40, STORE_JOIN = 16;	// the store's background going on right of its screen

StoreArt* GetStoreArt(LawnApp* theApp, Image* theBackground)
{
	static StoreArt gArt[2];
	MemoryImage* aSource = dynamic_cast<MemoryImage*>(theBackground);
	MemoryImage* aDay = dynamic_cast<MemoryImage*>(IMAGE_STORE_BACKGROUND);
	if (aSource == nullptr || aDay == nullptr || aSource->GetBits() == nullptr || aDay->GetBits() == nullptr ||
		aSource->mWidth < BOARD_WIDTH || aSource->mHeight < STORE_SKY_HEIGHT || aDay->mWidth != aSource->mWidth || aDay->mHeight != aSource->mHeight)
		return nullptr;
	StoreArt& anArt = gArt[aSource == aDay ? 0 : 1];
	if (anArt.mSource == aSource && anArt.mCrownEdge)
		return &anArt;
	anArt.mSource = aSource;
	const uint32_t* aBits = aSource->GetBits();
	const uint32_t* aDayBits = aDay->GetBits();
	int w = aSource->mWidth;
	auto R = [](uint32_t p) { return static_cast<int>((p >> 16) & 255); };
	auto G = [](uint32_t p) { return static_cast<int>((p >> 8) & 255); };
	auto B = [](uint32_t p) { return static_cast<int>(p & 255); };

	// the sky's column: right of the tree, the one that changes least from row to row
	long aBest = -1;
	for (int x = 360; x < w - 10; x++)
	{
		long aChange = 0;
		for (int y = 1; y < STORE_SKY_HEIGHT; y++)
		{
			uint32_t p = aBits[y * w + x], q = aBits[(y - 1) * w + x];
			aChange += std::abs(R(p) - R(q)) + std::abs(G(p) - G(q)) + std::abs(B(p) - B(q));
		}
		if (aBest < 0 || aChange < aBest)
		{
			aBest = aChange;
			anArt.mSkyColumn = x;
		}
	}

	// the crown's edge: leaves (green well above red and blue) and wood (red well above blue) kept, sky and clouds clear,
	// the pixels between them partly (so that no light rim of cloud stays on the leaves)
	anArt.mCrownEdge = std::make_unique<GLImage>(theApp->mGLInterface.get());
	anArt.mCrownEdge->Create(CROWN_EDGE_WIDTH, CROWN_EDGE_HEIGHT);
	uint32_t* anOut = anArt.mCrownEdge->GetBits();
	for (int y = 0; y < CROWN_EDGE_HEIGHT; y++)
	{
		for (int x = 0; x < CROWN_EDGE_WIDTH; x++)
		{
			uint32_t d = aDayBits[y * w + CROWN_EDGE_X + x];
			int aLeaf = G(d) - std::max(R(d), B(d));
			int aWood = std::min(R(d) - G(d), R(d) - B(d) - 20);
			int anAlpha = std::clamp(std::max((aLeaf - 15) * 255 / 20, aWood * 255 / 20), 0, 255);
			anOut[y * CROWN_EDGE_WIDTH + x] = anAlpha == 0 ? 0 : ((aBits[y * w + CROWN_EDGE_X + x] & 0x00FFFFFFu) | (static_cast<uint32_t>(anAlpha) << 24));
		}
	}
	anArt.mCrownEdge->mHasAlpha = true;
	anArt.mCrownEdge->mHasTrans = true;
	anArt.mCrownEdge->BitsChanged();
	return &anArt;
}

// Left of the store's screen (to theLeft, in the game's coordinates), from the store's own background: its sky above
// the fence; its tree's crown going on past the screen's edge and ending as a crown does (its natural edge, mirrored);
// its fence, the bushes and the pavement going on (a strip of them again and again, every other one mirrored, so that
// they join without a seam); then Crazy Dave whole, as the store draws him (its screen cuts him off at its left edge).
// Right of it (to theRight) its sky, its haze and the pavement going on, made of the background's strip there.
void DrawStoreScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow, double theLeft, double theRight)
{
	Image* aBackground = theApp->IsNight() ? IMAGE_STORE_BACKGROUNDNIGHT : IMAGE_STORE_BACKGROUND;
	StoreArt* anArt = GetStoreArt(theApp, aBackground);
	if (anArt == nullptr)
		return;
	int aLeft = static_cast<int>(std::floor(theLeft)) - 1;
	theApp->mGLInterface->PushTransform(theToWindow, false);
	g->DrawImage(aBackground, Rect(aLeft, 0, -aLeft + 1, STORE_SKY_HEIGHT), Rect(anArt->mSkyColumn, 0, 1, STORE_SKY_HEIGHT));
	theApp->mGLInterface->PopTransform();

	const int STRIP = 100;	// left of the tree's trunk, which stands behind the fence
	for (int i = 0; -(i * STRIP) > aLeft; i++)
	{
		// the strip's x from 0 to STRIP to [-(i + 1) STRIP, -i STRIP], mirrored for the one next to the screen and every other
		SexyMatrix3 aPlace;
		bool aMirrored = i % 2 == 0;
		aPlace.m00 = aMirrored ? -1.0f : 1.0f; aPlace.m01 = 0; aPlace.m02 = aMirrored ? static_cast<float>(-i * STRIP) : static_cast<float>(-(i + 1) * STRIP);
		aPlace.m10 = 0; aPlace.m11 = 1; aPlace.m12 = 0;
		aPlace.m20 = 0; aPlace.m21 = 0; aPlace.m22 = 1;
		theApp->mGLInterface->PushTransform(theToWindow * aPlace, false);
		g->DrawImage(aBackground, Rect(0, STORE_SKY_HEIGHT, STRIP, BOARD_HEIGHT - STORE_SKY_HEIGHT), Rect(0, STORE_SKY_HEIGHT, STRIP, BOARD_HEIGHT - STORE_SKY_HEIGHT));
		theApp->mGLInterface->PopTransform();
	}

	// the crown's edge mirrored so that its x CROWN_EDGE_X meets the screen's left edge: x' = CROWN_EDGE_X - x
	SexyMatrix3 aMirror;
	aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = static_cast<float>(CROWN_EDGE_X);
	aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = 0;
	aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
	theApp->mGLInterface->PushTransform(theToWindow * aMirror, false);
	g->DrawImage(anArt->mCrownEdge.get(), CROWN_EDGE_X, 0);
	theApp->mGLInterface->PopTransform();

	theApp->mGLInterface->PushTransform(theToWindow, false);
	Graphics aDaveG(*g);
	aDaveG.mTransX -= 42.0f;	// where the store draws him
	aDaveG.mTransY += 68.0f;
	theApp->DrawCrazyDave(&aDaveG);
	theApp->mGLInterface->PopTransform();

	// right of the screen its sky, its haze and the pavement going on (made of the background's strip at its right edge);
	// the car, which the screen cuts, fades into the background there (StoreFront)
	DrawExtension(g, theApp, theToWindow, GetExtension(theApp, aBackground, true, STORE_PERIOD, STORE_OVERLAP, STORE_JOIN), true, BOARD_WIDTH, 0,
		static_cast<float>(theRight));

}

// Over the store's screen: the far end of the car, which its picture cuts at the screen's edge, fading into what is
// behind it there (the background, which goes on past the edge), and the store's sign and coin bank over that
void DrawStoreFront(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	StoreScreen* aStore = dynamic_cast<StoreScreen*>(theApp->GetDialog(Dialogs::DIALOG_STORE));
	if (aStore == nullptr)
		return;
	Image* aBackground = theApp->IsNight() ? IMAGE_STORE_BACKGROUNDNIGHT : IMAGE_STORE_BACKGROUND;
	const int FADE = 64;
	theApp->mGLInterface->PushTransform(theToWindow, false);
	g->SetColorizeImages(true);
	for (int i = 0; i < FADE; i++)
	{
		int x = BOARD_WIDTH - FADE + i;
		float t = (i + 1.0f) / FADE;
		g->SetColor(Color(255, 255, 255, static_cast<int>(255 * t * t * (3 - 2 * t))));	// smooth at both ends
		g->DrawImage(aBackground, Rect(x, 0, 1, BOARD_HEIGHT), Rect(x, 0, 1, BOARD_HEIGHT));
	}
	g->SetColorizeImages(false);
	// over that again what hangs or stands there, as the store draws it: its sign, and the coin bank
	int aSignY = PvzpAnimateCurve(50, 110, aStore->mStoreTime, -150, 0, CURVE_EASE_IN_OUT);
	g->DrawImage(IMAGE_STORE_SIGN, 285, aSignY);
	g->DrawImage(IMAGE_COINBANK, 650, 559);
	g->SetColor(Color(180, 255, 90));
	g->SetFont(FONT_CONTINUUMBOLD14);
	std::string aCoinLabel = theApp->GetMoneyString(theApp->mPlayerInfo->mCoins);
	g->DrawString(aCoinLabel, 650 + 116 - FONT_CONTINUUMBOLD14->StringWidth(aCoinLabel), 559 + 24);
	theApp->mGLInterface->PopTransform();
}

// The fog's last column going on to the right past the screen's edge (Board::DrawFog draws one cel more than the
// column): more cels of the same column, as the board draws them, the first coming in from the screen's edge
void DrawFogBeyond(Graphics* g, Board* theBoard)
{
	bool aIs3DAccelerated = theBoard->mApp->Is3DAccelerated();
	Image* aImageFog = aIs3DAccelerated ? IMAGE_FOG : IMAGE_FOG_SOFTWARE;
	if (aImageFog == nullptr)
		return;
	constexpr uint32_t FOG_ANIM_PERIOD = 4500;
	float aTime = static_cast<float>(theBoard->mMainCounter % FOG_ANIM_PERIOD) * PI * 2;
	const int x = MAX_GRID_SIZE_X - 1;
	g->SetColorizeImages(true);
	for (int y = 0; y < MAX_GRID_SIZE_Y + 1; y++)
	{
		int aFadeAmount = theBoard->mGridCelFog[x][y];
		if (aFadeAmount == 0)
			continue;
		int aCelLook = theBoard->mGridCelLook[x][y % MAX_GRID_SIZE_Y];
		int aCelCol = aCelLook % 8;
		float aPosX = x * 80 + theBoard->mFogOffset - 15;
		float aPosY = y * 85 + 20;
		float aPhaseX = 6 * PI * x / MAX_GRID_SIZE_X;
		float aPhaseY = 6 * PI * y / (MAX_GRID_SIZE_Y + 1);
		float aMotion = 13 + 4 * sin(aTime / 900 + aPhaseY) + 8 * sin(aTime / 500 + aPhaseX);
		int aColorVariant = 255 - aCelLook * 1.5 - aMotion * 1.5;
		int aLightnessVariant = 255 - aCelLook - aMotion;
		if (!aIs3DAccelerated)
		{
			aPosX += 10;
			aPosY += 3;
			aCelCol = aCelLook % aImageFog->mNumCols;
			aColorVariant = 255;
			aLightnessVariant = 255;
		}
		g->SetColor(Color(aColorVariant, aColorVariant, aLightnessVariant, aFadeAmount));
		for (int k = 3; k <= 4; k++)
			g->DrawImageCel(aImageFog, static_cast<int>(aPosX + 80 * k), static_cast<int>(aPosY), aCelCol, 0);
		// the next cel, which begins on the screen (where the board does not draw it), coming in from the screen's edge
		// so that the fog there does not step
		const int RAMP = 60;
		int aCelX = static_cast<int>(aPosX + 160), aCelWidth = aImageFog->GetCelWidth(), aCelHeight = aImageFog->GetCelHeight();
		for (int c = std::max(0, BOARD_WIDTH - aCelX); c < aCelWidth; c++)
		{
			int x = aCelX + c;
			if (x >= BOARD_WIDTH + RAMP)
			{
				g->SetColor(Color(aColorVariant, aColorVariant, aLightnessVariant, aFadeAmount));
				g->DrawImage(aImageFog, Rect(x, static_cast<int>(aPosY), aCelWidth - c, aCelHeight), Rect(aCelCol * aCelWidth + c, 0, aCelWidth - c, aCelHeight));
				break;
			}
			g->SetColor(Color(aColorVariant, aColorVariant, aLightnessVariant, aFadeAmount * (x - BOARD_WIDTH) / RAMP));
			g->DrawImage(aImageFog, Rect(x, static_cast<int>(aPosY), 1, aCelHeight), Rect(aCelCol * aCelWidth + c, 0, 1, aCelHeight));
		}
	}
	g->SetColorizeImages(false);
}

// What of the level lies past the edges of the game's screen, in the board's coordinates (theFromBoard puts them in the
// window), over the backdrop, as the board draws them: the pool's water where the backdrop's picture has none (when the
// board is panned, as for the seed chooser); the lawn mowers near the edges (coming in, driving off); the zombies near
// the edges, their shadows first, in the order the board draws them; Dr. Zomboss; the fog over them, and its last column
// going on. The screen covers what of them is on it.
void DrawBeyond(Graphics* g, LawnApp* theApp, Board* theBoard, const SexyMatrix3& theFromBoard)
{
	theApp->mGLInterface->PushTransform(theFromBoard, false);
	// each drawn with a Graphics of its own, as some leave theirs changed (clipped)
	const Graphics aFresh(*g);
	if (theBoard->StageHasPool() && theBoard->mX != 0 && theApp->mPoolEffect)
	{
		Graphics aPoolG(aFresh);
		theApp->mPoolEffect->PoolEffectDraw(&aPoolG, theBoard->StageIsNight());
	}

	for (LawnMower* aMower : theBoard->mLawnMowers)
	{
		float x = aMower->mPosX + theBoard->mX;
		if (!aMower->mDead && (x < 60 || x > BOARD_WIDTH - 150))
		{
			Graphics aMowerG(aFresh);
			aMower->Draw(&aMowerG);
		}
	}

	std::vector<Zombie*> aZombies;
	for (Zombie* aZombie : theBoard->mZombies)
	{
		if (aZombie->mDead || aZombie->mZombieType == ZombieType::ZOMBIE_BOSS)
			continue;
		float x = aZombie->mX + theBoard->mX;
		if (x < 150 || x > BOARD_WIDTH - 250)
			aZombies.push_back(aZombie);
	}
	std::stable_sort(aZombies.begin(), aZombies.end(), [](Zombie* a, Zombie* b) { return a->mRenderOrder < b->mRenderOrder; });
	for (Zombie* aZombie : aZombies)
	{
		Graphics aZombieG(aFresh);
		if (aZombie->HasShadow() && aZombie->BeginDraw(&aZombieG))
			aZombie->DrawShadow(&aZombieG);
	}
	for (Zombie* aZombie : aZombies)
	{
		Graphics aZombieG(aFresh);
		if (aZombie->BeginDraw(&aZombieG))
			aZombie->Draw(&aZombieG);
	}

	if (Zombie* aBoss = theBoard->GetBossZombie())
	{
		for (BossPart aPart : { BossPart::BOSS_PART_BACK_LEG, BossPart::BOSS_PART_FRONT_LEG, BossPart::BOSS_PART_MAIN, BossPart::BOSS_PART_BACK_ARM,
			BossPart::BOSS_PART_FIREBALL })
		{
			if (aPart == BossPart::BOSS_PART_FIREBALL && theApp->ReanimationTryToGet(aBoss->mBossFireBallReanimID) == nullptr)
				continue;
			Graphics aBossG(aFresh);
			if (aBoss->BeginDraw(&aBossG))
				aBoss->DrawBossPart(&aBossG, aPart);
		}
	}

	if (theBoard->StageHasFog())
	{
		Graphics aFogG(aFresh);
		theBoard->DrawFog(&aFogG);
		DrawFogBeyond(&aFogG, theBoard);
	}
	theApp->mGLInterface->PopTransform();
}

// Past an end of the backdrop's image, out to the window's edge (theTile.mDest, in window points): the image's edge going
// on, softened, darkening towards the window's edge as a shadow there
void DrawEdge(Graphics* g, LawnApp* theApp, const PresentationTile& theTile)
{
	MemoryImage* aBackdrop = theApp->PhoneBackdrop();
	if (aBackdrop == nullptr)
		return;
	bool aRight = theTile.mDest.mX > 0.5;	// the left one begins at the window's left edge
	double aLeft = theTile.mDest.mX, aRightEdge = theTile.mDest.mX + theTile.mDest.mWidth;
	double aScale = theTile.mDest.mHeight / aBackdrop->mHeight;
	DrawEdgeFill(g, GetEdgeColumn(theApp, aBackdrop, aRight), aRight ? aLeft : aRightEdge, aRight ? aRightEdge : aLeft, theTile.mDest.mY, theTile.mDest.mHeight,
		aBackdrop, aScale, aRight ? aBackdrop->mWidth - 1 : 0);
}

// The board the cards lie on (theTile.mDest, in window points; theTile.mSource its size in its units, the cards'): the
// packets' part of the seed bank's wooden board turned upright, its rim along its right edge (along its left for the
// board right of a garden, mOverlayIndex 1), going on past the window's other edges
void DrawBoard(Graphics* g, LawnApp* theApp, const PresentationTile& theTile)
{
	if (theTile.mOverlayIndex == 2)
	{
		// under the notch or the Dynamic Island, left of the board: black, as the phone's edge around it
		g->SetColor(Color::Black);
		g->FillRect(static_cast<int>(std::floor(theTile.mDest.mX)), static_cast<int>(std::floor(theTile.mDest.mY)),
			static_cast<int>(std::ceil(theTile.mDest.mWidth)) + 1, static_cast<int>(std::ceil(theTile.mDest.mHeight)) + 1);
		return;
	}
	Image* aBank = IMAGE_SEEDBANK;
	if (aBank == nullptr || theTile.mSource.mWidth <= 0 || theTile.mSource.mHeight <= 0)
		return;
	// of the bank's picture: its rim along its top with the shading below it, a row of its flat wood (all alike), the
	// packets' part
	const int RIM_AND_SHADE = 20, FLAT_ROW = 40, FROM = 150, TO = 430;
	float s = static_cast<float>(theTile.mDest.mWidth / theTile.mSource.mWidth);
	int aLength = static_cast<int>(std::ceil(theTile.mSource.mHeight)) + 1, aThickness = static_cast<int>(std::ceil(theTile.mSource.mWidth)) + 1;
	// along the bank down the window, across it from its rim leftwards (rightwards for a board with its rim on its left)
	bool aRimLeft = theTile.mOverlayIndex == 1;
	SexyMatrix3 aTurn;
	aTurn.m00 = 0; aTurn.m01 = aRimLeft ? s : -s; aTurn.m02 = static_cast<float>(aRimLeft ? theTile.mDest.mX : theTile.mDest.mX + theTile.mDest.mWidth);
	aTurn.m10 = s; aTurn.m11 = 0; aTurn.m12 = static_cast<float>(theTile.mDest.mY);
	aTurn.m20 = 0; aTurn.m21 = 0; aTurn.m22 = 1;
	theApp->mGLInterface->PushTransform(aTurn, false);
	// the rim and its shading as they are, the flat wood past them (not the bank's own width stretched, which would
	// smear its dents across a wide board)
	g->DrawImage(aBank, Rect(0, 0, aLength, RIM_AND_SHADE), Rect(FROM, 0, TO - FROM, RIM_AND_SHADE));
	if (aThickness > RIM_AND_SHADE)
		g->DrawImage(aBank, Rect(0, RIM_AND_SHADE, aLength, aThickness - RIM_AND_SHADE), Rect(FROM, FLAT_ROW, TO - FROM, 1));
	theApp->mGLInterface->PopTransform();
}

// The rain of the stormy night past the game's screen (in what theToWindow takes from its coordinates), as the game
// draws it over its screen (Challenge::DrawRain), its rows of drops going on further left and right
void DrawRainBeyond(Graphics* g, LawnApp* theApp, Board* theBoard, const SexyMatrix3& theToWindow)
{
	if (theBoard->mCutScene->IsBeforePreloading() || !theApp->Is3DAccelerated() || IMAGE_RAIN == nullptr)
		return;
	int aBoardOffsetX = theBoard->mX / 100 * -100;
	const uint32_t aTime = theBoard->mEffectCounter;
	int aTimeOffsetXEst = PvzpAnimateCurve(0, 100, static_cast<int>(aTime % 100u), 0, -100, CURVE_LINEAR);
	int aTimeOffsetYEst = PvzpAnimateCurve(0, 20, static_cast<int>(aTime % 20u), -100, 0, CURVE_LINEAR);
	theApp->mGLInterface->PushTransform(theToWindow, false);
	for (int aHorCnt = 16; aHorCnt > -8; aHorCnt--)
		for (int aVerCnt = 7; aVerCnt > 0; aVerCnt--)
			g->DrawImage(IMAGE_RAIN, aTimeOffsetXEst + 100 * aHorCnt + aBoardOffsetX, aTimeOffsetYEst + 100 * aVerCnt);
	float aTimeOffsetXCls = PvzpAnimateCurve(0, 161, static_cast<int>(aTime % 161u), 0, -100, CURVE_LINEAR);
	float aTimeOffsetYCls = PvzpAnimateCurve(0, 33, static_cast<int>(aTime % 33u), -100, 0, CURVE_LINEAR);
	for (int aHorCnt = -6; aHorCnt < 12; aHorCnt++)
		for (int aVerCnt = 0; aVerCnt < 7; aVerCnt++)
		{
			const float SCALE = 1.5f;
			PvzpDrawImageScaledF(g, IMAGE_RAIN, (aHorCnt * 100 + aTimeOffsetXCls) * SCALE + aBoardOffsetX, (aVerCnt * 100 + aTimeOffsetYCls) * SCALE, SCALE, SCALE);
		}
	theApp->mGLInterface->PopTransform();
}

// The title over the whole window: its picture covering it (cut at the top and bottom, where there is sky and lawn), and
// its logo and loading bar over it where the game's screen (theTile) has them, at their size
void DrawTitle(Graphics* g, LawnApp* theApp, const PresentationTile& theTile)
{
	TitleScreen* aTitle = theApp->mTitleScreen.get();
	if (aTitle == nullptr || !aTitle->ShowsTitle() || IMAGE_TITLESCREEN == nullptr)
		return;
	int aWindowWidth = 0, aWindowHeight = 0;
	SDL_GetWindowSize(theApp->mWindow.get(), &aWindowWidth, &aWindowHeight);
	float aCover = std::max(static_cast<float>(aWindowWidth) / IMAGE_TITLESCREEN->mWidth, static_cast<float>(aWindowHeight) / IMAGE_TITLESCREEN->mHeight);
	SexyTransform2D aPicture;
	aPicture.Scale(aCover, aCover);
	aPicture.Translate((aWindowWidth - IMAGE_TITLESCREEN->mWidth * aCover) / 2, (aWindowHeight - IMAGE_TITLESCREEN->mHeight * aCover) / 2);
	theApp->mGLInterface->PushTransform(aPicture, false);
	{
		Graphics aPictureG(*g);
		aPictureG.DrawImage(IMAGE_TITLESCREEN, 0, 0);
	}
	theApp->mGLInterface->PopTransform();
	theApp->mGLInterface->PushTransform(ToWindow(theTile), false);
	{
		Graphics aForegroundG(*g);
		aTitle->DrawForeground(&aForegroundG);
	}
	theApp->mGLInterface->PopTransform();
}

// The dark band behind a message, which the message draws across the game's screen (theTile.mDest), going on across the
// window, in window points
void DrawBand(Graphics* g, Board* theBoard, const PresentationTile& theTile)
{
	int y = 0, aHeight = 0;
	if (!theBoard->mAdvice || !theBoard->mAdvice->GetBand(y, aHeight))
		return;
	double aScale = theTile.mDest.mHeight / theTile.mSource.mHeight;
	double aTop = theTile.mDest.mY + (y - theTile.mSource.mY) * aScale, aBottom = aTop + aHeight * aScale;
	int t = static_cast<int>(std::lround(aTop)), b = static_cast<int>(std::lround(aBottom));
	int aLeft = static_cast<int>(std::lround(theTile.mDest.mX)), aRight = static_cast<int>(std::lround(theTile.mDest.mX + theTile.mDest.mWidth));
	g->SetColor(Color(0, 0, 0, 128));
	g->FillRect(-10000, t, aLeft + 10000, b - t);
	g->FillRect(aRight, t, 10000, b - t);
}
}

void Overlay(LawnApp* theApp, const PresentationTile& theTile)
{
	Board* aBoard = theApp->mBoard;
	PhoneLayout::Kind aKind = static_cast<PhoneLayout::Kind>(theTile.mOverlay);
	if (theApp->mWidgetManager == nullptr || theApp->mWidgetManager->mImage == nullptr)
		return;
	if (aKind != PhoneLayout::Kind::Surround && aKind != PhoneLayout::Kind::Title && aKind != PhoneLayout::Kind::StoreFront && aKind != PhoneLayout::Kind::Edge &&
		aKind != PhoneLayout::Kind::Board && (aBoard == nullptr || !aBoard->mSeedBank))
		return;

	// what is drawn in its own units, scaled and moved into the tile through the transform of the GL interface
	SexyTransform2D aTransform;
	switch (aKind)
	{
	case PhoneLayout::Kind::Card:
		aTransform.Scale(static_cast<float>(theTile.mDest.mWidth / PhoneLayout::CARD_WIDTH), static_cast<float>(theTile.mDest.mHeight / PhoneLayout::CARD_HEIGHT));
		break;
	case PhoneLayout::Kind::SunCounter:
		aTransform.Scale(static_cast<float>(theTile.mDest.mWidth / PhoneLayout::SUN_COUNTER_WIDTH), static_cast<float>(theTile.mDest.mHeight / PhoneLayout::SUN_COUNTER_HEIGHT));
		break;
	case PhoneLayout::Kind::Conveyor:
	{
		// along x and across y in card units, turned to run down the tile: x to y, y to the left from its right edge
		double aScale = theTile.mDest.mWidth / (PhoneLayout::CARD_WIDTH + 2 * PhoneLayout::CONVEYOR_PAD);
		SexyMatrix3 aTurn;
		aTurn.m00 = 0; aTurn.m01 = -1; aTurn.m02 = static_cast<float>(theTile.mDest.mWidth / aScale);
		aTurn.m10 = 1; aTurn.m11 = 0; aTurn.m12 = 0;
		aTurn.m20 = 0; aTurn.m21 = 0; aTurn.m22 = 1;
		aTransform = aTurn;
		aTransform.Scale(static_cast<float>(aScale), static_cast<float>(aScale));
		break;
	}
	case PhoneLayout::Kind::Weather:
	case PhoneLayout::Kind::Surround:
	case PhoneLayout::Kind::Beyond:
	case PhoneLayout::Kind::Band:
	case PhoneLayout::Kind::Title:
	case PhoneLayout::Kind::StoreFront:
	case PhoneLayout::Kind::Edge:
	case PhoneLayout::Kind::Board:
		break;	// in window points, clipped to the tile; the sceneries and what is beyond set up their own transforms
	case PhoneLayout::Kind::Meter:
	case PhoneLayout::Kind::Menu:
	case PhoneLayout::Kind::Shovel:
		// drawn by the game where it draws it in its screen, theTile.mSource
		aTransform.Translate(static_cast<float>(-theTile.mSource.mX), static_cast<float>(-theTile.mSource.mY));
		aTransform.Scale(static_cast<float>(theTile.mDest.mWidth / theTile.mSource.mWidth), static_cast<float>(theTile.mDest.mHeight / theTile.mSource.mHeight));
		break;
	default:
		return;
	}
	if (aKind != PhoneLayout::Kind::Weather && aKind != PhoneLayout::Kind::Surround && aKind != PhoneLayout::Kind::Beyond && aKind != PhoneLayout::Kind::Band &&
		aKind != PhoneLayout::Kind::Title && aKind != PhoneLayout::Kind::StoreFront && aKind != PhoneLayout::Kind::Edge && aKind != PhoneLayout::Kind::Board)
		aTransform.Translate(static_cast<float>(theTile.mDest.mX), static_cast<float>(theTile.mDest.mY));

	Graphics g(theApp->mWidgetManager->mImage);
	g.mClipRect = Rect(-100000, -100000, 200000, 200000);	// the transform puts it in the window; nothing to clip
	if (aKind == PhoneLayout::Kind::Weather)
	{
		// the pixels whose middles are in it: not those of the screen beside it, which has its own darkness
		auto Edge = [](double v) { return static_cast<int>(std::ceil(v - 0.5)); };
		int aLeft = Edge(theTile.mDest.mX), aTop = Edge(theTile.mDest.mY);
		g.mClipRect = Rect(aLeft, aTop, Edge(theTile.mDest.mX + theTile.mDest.mWidth) - aLeft, Edge(theTile.mDest.mY + theTile.mDest.mHeight) - aTop);
	}
	g.SetLinearBlend(true);
	theApp->mGLInterface->PushTransform(aTransform, false);
	switch (aKind)
	{
	case PhoneLayout::Kind::Card:
		if (theTile.mOverlayIndex >= 0 && theTile.mOverlayIndex < aBoard->mSeedBank->mNumPackets &&
			aBoard->mSeedBank->mSeedPackets[theTile.mOverlayIndex].mPacketType != SeedType::SEED_NONE)
		{
			// on a conveyor belt, cut where its end begins: the cards come in from under it
			for (const PresentationTile& t : theApp->mGLInterface->GetPresentationTiles())
			{
				if (t.mOverlay != static_cast<int>(PhoneLayout::Kind::Conveyor))
					continue;
				double aBeltScale = t.mDest.mWidth / (PhoneLayout::CARD_WIDTH + 2 * PhoneLayout::CONVEYOR_PAD);
				double anEnd = t.mDest.mY + t.mDest.mHeight - CONVEYOR_END * aBeltScale;
				double aCardScale = theTile.mDest.mHeight / PhoneLayout::CARD_HEIGHT;
				g.ClipRect(-1000, -1000, 2000, static_cast<int>(std::floor((anEnd - theTile.mDest.mY) / aCardScale)) + 1000);
			}
			DrawCard(&g, aBoard->mSeedBank->mSeedPackets[theTile.mOverlayIndex]);
		}
		break;
	case PhoneLayout::Kind::SunCounter:
		DrawSunCounter(&g, aBoard);
		break;
	case PhoneLayout::Kind::Surround:
		switch (theTile.mOverlayIndex)
		{
		case PhoneLayout::SURROUND_MENU:	DrawMenuScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_STORE:
		{
			int aWindowWidth = 0, aWindowHeight = 0;
			SDL_GetWindowSize(theApp->mWindow.get(), &aWindowWidth, &aWindowHeight);
			double aScale = theTile.mDest.mWidth / theTile.mSource.mWidth;
			DrawStoreScene(&g, theApp, ToWindow(theTile), theTile.mSource.mX - theTile.mDest.mX / aScale,
				theTile.mSource.mX + (aWindowWidth - theTile.mDest.mX) / aScale);
		}
			break;
		case PhoneLayout::SURROUND_PAGE:	DrawPageScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_NOTE:	DrawNoteScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_TREE:	DrawTreeScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_CREDITS:
		{
			int aWindowWidth = 0, aWindowHeight = 0;
			SDL_GetWindowSize(theApp->mWindow.get(), &aWindowWidth, &aWindowHeight);
			DrawCreditsScene(&g, theApp, ToWindow(theTile), 0, aWindowWidth);
		}
			break;
		default:							break;
		}
		break;
	case PhoneLayout::Kind::Band:
		DrawBand(&g, aBoard, theTile);
		break;
	case PhoneLayout::Kind::Title:
		DrawTitle(&g, theApp, theTile);
		break;
	case PhoneLayout::Kind::StoreFront:
		DrawStoreFront(&g, theApp, ToWindow(theTile));
		break;
	case PhoneLayout::Kind::Beyond:
		if (aBoard)
		{
			SexyTransform2D aFromBoard = ToWindow(theTile);
			SexyTransform2D aShift;
			aShift.Translate(static_cast<float>(aBoard->mX), static_cast<float>(aBoard->mY));
			DrawBeyond(&g, theApp, aBoard, aFromBoard * aShift);
		}
		break;
	case PhoneLayout::Kind::Weather:
		if (aBoard->mChallenge && theApp->IsStormyNightLevel())
		{
			// its rain (clipped to the tile, in the game's coordinates) and its darkness, as over the game's screen
			Graphics aRainG(g);
			const FRect& aSource = theTile.mSource;
			aRainG.mClipRect = Rect(static_cast<int>(std::floor(aSource.mX)), static_cast<int>(std::floor(aSource.mY)), static_cast<int>(std::ceil(aSource.mWidth)),
				static_cast<int>(std::ceil(aSource.mHeight)));
			DrawRainBeyond(&aRainG, theApp, aBoard, ToWindow(theTile));
			aBoard->mChallenge->DrawStormDarkness(&g);
		}
		break;
	case PhoneLayout::Kind::Edge:
		DrawEdge(&g, theApp, theTile);
		break;
	case PhoneLayout::Kind::Board:
		DrawBoard(&g, theApp, theTile);
		break;
	case PhoneLayout::Kind::Conveyor:
	{
		double aScale = theTile.mDest.mWidth / (PhoneLayout::CARD_WIDTH + 2 * PhoneLayout::CONVEYOR_PAD);
		DrawConveyor(&g, aBoard, static_cast<int>(std::ceil(theTile.mDest.mHeight / aScale)), static_cast<int>(PhoneLayout::CARD_WIDTH + 2 * PhoneLayout::CONVEYOR_PAD));
		break;
	}
	case PhoneLayout::Kind::Meter:
		theApp->mDrawingPhoneOverlay = true;
		aBoard->DrawProgressMeter(&g);
		theApp->mDrawingPhoneOverlay = false;
		break;
	case PhoneLayout::Kind::Menu:
		if (aBoard->mMenuButton)
			aBoard->mMenuButton->Draw(&g);
		break;
	case PhoneLayout::Kind::Shovel:
		theApp->mDrawingPhoneOverlay = true;
		aBoard->DrawShovel(&g);
		theApp->mDrawingPhoneOverlay = false;
		break;
	default:
		break;
	}
	theApp->mGLInterface->PopTransform();
}

}
