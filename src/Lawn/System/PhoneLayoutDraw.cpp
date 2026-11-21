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
#include "Lawn/SeedPacket.h"
#include "Lawn/Widget/GameButton.h"
#include "Lawn/Widget/GameSelector.h"
#include "PvzpLib/EffectSystem.h"
#include "PvzpLib/Reanimator.h"
#include "PvzpLib/PvzpCommon.h"
#include "graphics/Font.h"
#include "graphics/GLInterface.h"
#include "graphics/Graphics.h"
#include "misc/SexyMatrix.h"
#include "widget/WidgetManager.h"
#include <algorithm>
#include <cmath>
#include <span>
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
		SeedPacketDrawSeed(&aPlantG, 25 - 25 * k, 24 - 29 * k, thePacket.mPacketType, thePacket.mImitaterType, aOffsetX * k, aOffsetY * k, aScale);
	}

	// the cost, in the strip of the packet with its sun, in the bottom right corner
	if (aDrawCost)
	{
		const int aStripX = W - 47, aStripY = H - 16;
		Piece(g, IMAGE_SEEDS, aStripX, aStripY, 46, 14, aCel + 2, 54, 46, 14);
		std::string aCost = SeedPacketCostString(thePacket.mPacketType, thePacket.mImitaterType, true);
		_Font* aFont = FONT_CONTINUUMBOLD14;
		const float aTextScale = 0.8f;
		ScaledString(g, aFont, aCost, aStripX + 31 - aFont->StringWidth(aCost) * aTextScale, aStripY + 7 + aFont->GetAscent() * aTextScale / 2, aTextScale, Color::Black);
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
void DrawConveyor(Graphics* g, Board* theBoard, int theLength, int theWidth)
{
	Image* aPanel = IMAGE_CONVEYORBELT_BACKDROP;	// 516x86, a metal frame
	const int aColumns[4] = { 0, 8, 508, 516 };
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

// The main menu's scenery around its screen, in the game's coordinates (the transform puts them in the window): its
// sky and its layers mirrored at the edges of the screen, the tree on the left and the hills on the right (not the
// tombstone: that would make a second one), so that they go on without a seam; and its clouds where they are, drifting
// past the edges. Around the main menu, and around pages (the almanac, the lists of mini-games) the same.
void DrawMenuScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
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
	// in the game's order: the sky, the clouds, the layers in front of them
	const char* const SKY[] = { "SelectorScreen_BG" };
	const char* const LEFT[] = { "SelectorScreen_BG_Left" };
	const char* const RIGHT[] = { "SelectorScreen_BG_Center" };
	auto DrawMirrored = [&](std::span<const char* const> theLeftTracks, std::span<const char* const> theRightTracks)
	{
		for (int aSide = 0; aSide < 2; aSide++)
		{
			// mirrored at x 0 or at x BOARD_WIDTH
			SexyMatrix3 aMirror;
			aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = aSide == 0 ? 0.0f : 2.0f * BOARD_WIDTH;
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
	DrawMirrored(LEFT, RIGHT);
	if (anOwn)
		anOwn->ReanimationDie();
}

}

void Overlay(LawnApp* theApp, const PresentationTile& theTile)
{
	Board* aBoard = theApp->mBoard;
	PhoneLayout::Kind aKind = static_cast<PhoneLayout::Kind>(theTile.mOverlay);
	if (theApp->mWidgetManager == nullptr || theApp->mWidgetManager->mImage == nullptr)
		return;
	if (aKind != PhoneLayout::Kind::MenuScene && aKind != PhoneLayout::Kind::Mirrored && (aBoard == nullptr || !aBoard->mSeedBank))
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
	case PhoneLayout::Kind::MenuScene:
	case PhoneLayout::Kind::Mirrored:
		break;	// in window points, clipped to the tile; the scenery and the mirrored background set up their own transforms
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
	if (aKind != PhoneLayout::Kind::Weather && aKind != PhoneLayout::Kind::MenuScene && aKind != PhoneLayout::Kind::Mirrored)
		aTransform.Translate(static_cast<float>(theTile.mDest.mX), static_cast<float>(theTile.mDest.mY));

	Graphics g(theApp->mWidgetManager->mImage);
	g.mClipRect = Rect(-100000, -100000, 200000, 200000);	// the transform puts it in the window; nothing to clip
	if (aKind == PhoneLayout::Kind::Weather)
		g.mClipRect = Rect(static_cast<int>(std::floor(theTile.mDest.mX)), static_cast<int>(std::floor(theTile.mDest.mY)),
			static_cast<int>(std::ceil(theTile.mDest.mWidth)) + 1, static_cast<int>(std::ceil(theTile.mDest.mHeight)) + 1);
	g.SetLinearBlend(true);
	theApp->mGLInterface->PushTransform(aTransform, false);
	switch (aKind)
	{
	case PhoneLayout::Kind::Card:
		if (theTile.mOverlayIndex >= 0 && theTile.mOverlayIndex < aBoard->mSeedBank->mNumPackets &&
			aBoard->mSeedBank->mSeedPackets[theTile.mOverlayIndex].mPacketType != SeedType::SEED_NONE)
			DrawCard(&g, aBoard->mSeedBank->mSeedPackets[theTile.mOverlayIndex]);
		break;
	case PhoneLayout::Kind::SunCounter:
		DrawSunCounter(&g, aBoard);
		break;
	case PhoneLayout::Kind::Mirrored:
	{
		// the store's background mirrored at both edges of its screen
		Image* aBackground = theTile.mOverlayIndex == 1 ? IMAGE_STORE_BACKGROUNDNIGHT : IMAGE_STORE_BACKGROUND;
		if (aBackground == nullptr)
			break;
		float aScale = static_cast<float>(theTile.mDest.mWidth / theTile.mSource.mWidth);
		for (int aSide = 0; aSide < 2; aSide++)
		{
			SexyTransform2D aMirror;
			aMirror.m00 = -1;
			aMirror.m02 = aSide == 0 ? 0.0f : 2.0f * static_cast<float>(theTile.mSource.mWidth);
			aMirror.Scale(aScale, aScale);
			aMirror.Translate(static_cast<float>(theTile.mDest.mX), static_cast<float>(theTile.mDest.mY));
			theApp->mGLInterface->PushTransform(aMirror, false);
			g.DrawImage(aBackground, 0, 0);
			theApp->mGLInterface->PopTransform();
		}
		break;
	}
	case PhoneLayout::Kind::MenuScene:
	{
		SexyTransform2D aToWindow;
		aToWindow.Scale(static_cast<float>(theTile.mDest.mWidth / theTile.mSource.mWidth), static_cast<float>(theTile.mDest.mHeight / theTile.mSource.mHeight));
		aToWindow.Translate(static_cast<float>(theTile.mDest.mX - theTile.mSource.mX * theTile.mDest.mWidth / theTile.mSource.mWidth),
			static_cast<float>(theTile.mDest.mY - theTile.mSource.mY * theTile.mDest.mHeight / theTile.mSource.mHeight));
		DrawMenuScene(&g, theApp, aToWindow);
		break;
	}
	case PhoneLayout::Kind::Weather:
		if (aBoard->mChallenge && theApp->IsStormyNightLevel())
			aBoard->mChallenge->DrawStormDarkness(&g);
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
