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
#include "Lawn/SeedPacket.h"
#include "Lawn/Widget/GameButton.h"
#include "PvzpLib/PvzpCommon.h"
#include "graphics/Font.h"
#include "graphics/GLInterface.h"
#include "graphics/Graphics.h"
#include "misc/SexyMatrix.h"
#include "widget/WidgetManager.h"
#include <algorithm>
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
	// the paper, its ends as they are (with the tape) and its middle stretched
	const int aPaperX = H, aPaperY = 6, aPaperWidth = W - H - 4, aPaperHeight = H - 12;
	const int aPaperColumns[4] = { 9, 21, 57, 69 };
	const int aPaperRows[4] = { 57, 57, 85, 85 };
	Stretched(g, aBank, aPaperX, aPaperY, aPaperWidth, aPaperHeight, aPaperColumns, aPaperRows);

	std::string aMoney = std::to_string(std::max(theBoard->mSunMoney, 0));
	Color aColor(0, 0, 0);
	if (theBoard->mOutOfMoneyCounter > 0 && theBoard->mOutOfMoneyCounter % 20 < 10)
		aColor = Color(255, 0, 0);
	_Font* aFont = FONT_CONTINUUMBOLD14;
	const float aScale = 1.35f;
	float aWidth = aFont->StringWidth(aMoney) * aScale;
	ScaledString(g, aFont, aMoney, aPaperX + (aPaperWidth - aWidth) / 2, aPaperY + aPaperHeight / 2 + aFont->GetAscent() * aScale / 2 - 1, aScale, aColor);
}

}

void Overlay(LawnApp* theApp, const PresentationTile& theTile)
{
	Board* aBoard = theApp->mBoard;
	if (aBoard == nullptr || !aBoard->mSeedBank || theApp->mWidgetManager == nullptr || theApp->mWidgetManager->mImage == nullptr)
		return;
	PhoneLayout::Kind aKind = static_cast<PhoneLayout::Kind>(theTile.mOverlay);

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
	aTransform.Translate(static_cast<float>(theTile.mDest.mX), static_cast<float>(theTile.mDest.mY));

	Graphics g(theApp->mWidgetManager->mImage);
	g.mClipRect = Rect(-100000, -100000, 200000, 200000);	// the transform puts it in the window; nothing to clip
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
