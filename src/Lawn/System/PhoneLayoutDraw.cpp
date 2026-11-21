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
#include "Lawn/Zombie.h"
#include "Lawn/LawnMower.h"
#include "Lawn/MessageWidget.h"
#include "Lawn/System/PoolEffect.h"
#include "Lawn/Widget/GameButton.h"
#include "Lawn/Widget/AchievementsScreen.h"
#include "Lawn/Widget/AwardScreen.h"
#include "Lawn/Widget/CreditScreen.h"
#include "Lawn/Widget/GameSelector.h"
#include "Lawn/Widget/TitleScreen.h"
#include "Lawn/Widget/ZombatarWidget.h"
#include "PvzpLib/EffectSystem.h"
#include "PvzpLib/Reanimator.h"
#include "PvzpLib/PvzpCommon.h"
#include "graphics/Font.h"
#include "graphics/GLImage.h"
#include "graphics/GLInterface.h"
#include "graphics/Graphics.h"
#include "misc/SexyMatrix.h"
#include "widget/WidgetManager.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
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

// The main menu's scenery around its screen, in the game's coordinates (the transform puts them in the window): its
// sky and its layers mirrored at the edges of the screen, the tree on the left and the hills on the right (not the
// tombstone: that would make a second one), so that they go on without a seam; and its clouds where they are, drifting
// past the edges. Around the main menu, and around pages (the almanac, the lists of mini-games) the same.
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

	// the achievements below the main menu: their earth going on left of them (a strip at their left edge, left of their
	// title's stones, again and again)
	float aLeftmost = -2.0f * BOARD_WIDTH;
	if (aSelector && aSelector->mAchievementsWidget && aSelector->mAchievementsWidget->mY < BOARD_HEIGHT && IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG)
	{
		const AchievementsWidget& anAchievements = *aSelector->mAchievementsWidget;
		const int STRIP_X = 4, STRIP = 86;
		float x = static_cast<float>(anAchievements.mX), y = static_cast<float>(anAchievements.mY);
		int aHeight = IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG->mHeight;
		if (y + aHeight > 0)
			DrawStripsLeft(g, theApp, theToWindowStill, IMAGE_SELECTORSCREEN_ACHIEVEMENTS_BG, STRIP_X, STRIP, x, y, aLeftmost);
		if (IMAGE_ACHEESEMENTS_HOLE_TILE)
		{
			int aTile = IMAGE_ACHEESEMENTS_HOLE_TILE->mHeight;
			for (int i = 1; i <= 70; i++)
			{
				float aTileY = y + static_cast<float>(aHeight * i);
				if (aTileY > BOARD_HEIGHT)
					break;
				if (aTileY + aTile > 0)
					DrawStripsLeft(g, theApp, theToWindowStill, IMAGE_ACHEESEMENTS_HOLE_TILE, STRIP_X, STRIP, x, aTileY, aLeftmost);
			}
		}
	}

	// Zombatar right of the main menu: its graveyard going on left of it (its left part, left of its BACK stone: the
	// obelisk at its edge, its wall, the mound and the grass, again and again)
	if (aSelector && aSelector->mZombatarWidget && aSelector->mZombatarWidget->mX < BOARD_WIDTH && IMAGE_ZOMBATAR_MAIN_BG)
	{
		const ZombatarWidget& aZombatar = *aSelector->mZombatarWidget;
		DrawStripsLeft(g, theApp, theToWindowStill, IMAGE_ZOMBATAR_MAIN_BG, 0, 280, static_cast<float>(aZombatar.mX), static_cast<float>(aZombatar.mY), aLeftmost);
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

// The Zen Garden's own picture (on the board, at its left edge) going on past the screen's edges, mirrored at them
void DrawGardenScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	Board* aBoard = theApp->mBoard;
	if (aBoard == nullptr)
		return;
	Image* anImage =
		aBoard->mBackground == BackgroundType::BACKGROUND_GREENHOUSE ? IMAGE_BACKGROUND_GREENHOUSE :
		aBoard->mBackground == BackgroundType::BACKGROUND_MUSHROOM_GARDEN ? IMAGE_BACKGROUND_MUSHROOMGARDEN :
		aBoard->mBackground == BackgroundType::BACKGROUND_ZOMBIQUARIUM ? IMAGE_AQUARIUM1 : nullptr;
	if (anImage == nullptr)
		return;
	for (int aSide = 0; aSide < 2; aSide++)
	{
		float anEdge = static_cast<float>(aBoard->mX + (aSide == 0 ? 0 : anImage->mWidth));
		SexyMatrix3 aMirror;
		aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = 2 * anEdge - aBoard->mX;
		aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = static_cast<float>(aBoard->mY);
		aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
		theApp->mGLInterface->PushTransform(theToWindow * aMirror, false);
		g->DrawImage(anImage, 0, 0);
		theApp->mGLInterface->PopTransform();
	}
}

// The Tree of Wisdom's sky and hill going on past the screen's edges, mirrored at them, its clouds where they are
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
	auto DrawMirrored = [&](int theRenderGroup)
	{
		for (int aSide = 0; aSide < 2; aSide++)
		{
			SexyMatrix3 aMirror;
			aMirror.m00 = -1; aMirror.m01 = 0; aMirror.m02 = aSide == 0 ? 0.0f : 2.0f * BOARD_WIDTH;
			aMirror.m10 = 0; aMirror.m11 = 1; aMirror.m12 = 0;
			aMirror.m20 = 0; aMirror.m21 = 0; aMirror.m22 = 1;
			theApp->mGLInterface->PushTransform(aToWindow * aMirror, false);
			Graphics aTreeG(*g);
			bool anOverlay = aTree->mEnableExtraOverlayDraw;
			aTree->mEnableExtraOverlayDraw = false;
			aTree->DrawRenderGroup(&aTreeG, theRenderGroup);
			aTree->mEnableExtraOverlayDraw = anOverlay;
			theApp->mGLInterface->PopTransform();
		}
	};
	DrawMirrored(0);	// the sky
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
	DrawMirrored(3);	// the hill
}

// The credits as they draw themselves, past the screen's edges (their scenes are larger than the screen; where they
// end, the blurred copy under them shows); the screen covers what is on it
void DrawCreditsScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow)
{
	CreditScreen* aCredits = theApp->mCreditScreen.get();
	if (aCredits == nullptr)
		return;
	SexyTransform2D aPlace;
	aPlace.Translate(static_cast<float>(aCredits->mX), static_cast<float>(aCredits->mY));
	theApp->mGLInterface->PushTransform(theToWindow * aPlace, false);
	Graphics aCreditsG(*g);
	aCredits->Draw(&aCreditsG);
	theApp->mGLInterface->PopTransform();
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
// they join without a seam); then Crazy Dave whole, as the store draws him (its screen cuts him off at its left edge)
void DrawStoreScene(Graphics* g, LawnApp* theApp, const SexyMatrix3& theToWindow, double theLeft)
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

	// and the crown at the screen's left edge mirrored over it, fading out, so that the two join softly
	const int FADE = 36, SLIVERS = 12;
	SexyMatrix3 aFlip;
	aFlip.m00 = -1; aFlip.m01 = 0; aFlip.m02 = 0;
	aFlip.m10 = 0; aFlip.m11 = 1; aFlip.m12 = 0;
	aFlip.m20 = 0; aFlip.m21 = 0; aFlip.m22 = 1;
	theApp->mGLInterface->PushTransform(theToWindow * aFlip, false);
	g->SetColorizeImages(true);
	for (int i = 0; i < SLIVERS; i++)
	{
		int x = i * FADE / SLIVERS, aWidth = (i + 1) * FADE / SLIVERS - x;
		g->SetColor(Color(255, 255, 255, 255 * (SLIVERS - i) / (SLIVERS + 1)));
		g->DrawImage(aBackground, Rect(x, 0, aWidth, CROWN_EDGE_HEIGHT), Rect(x, 0, aWidth, CROWN_EDGE_HEIGHT));
	}
	g->SetColorizeImages(false);
	theApp->mGLInterface->PopTransform();

	theApp->mGLInterface->PushTransform(theToWindow, false);
	Graphics aDaveG(*g);
	aDaveG.mTransX -= 42.0f;	// where the store draws him
	aDaveG.mTransY += 68.0f;
	theApp->DrawCrazyDave(&aDaveG);
	theApp->mGLInterface->PopTransform();
}

// The fog's last column going on to the right past the screen's edge (Board::DrawFog draws one cel more than the
// column): more cels of the same column, as the board draws them
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
		for (int k = 2; k <= 4; k++)
			g->DrawImageCel(aImageFog, static_cast<int>(aPosX + 80 * k), static_cast<int>(aPosY), aCelCol, 0);
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
	if (aKind != PhoneLayout::Kind::Surround && aKind != PhoneLayout::Kind::Title && (aBoard == nullptr || !aBoard->mSeedBank))
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
		aKind != PhoneLayout::Kind::Title)
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
	case PhoneLayout::Kind::Surround:
		switch (theTile.mOverlayIndex)
		{
		case PhoneLayout::SURROUND_MENU:	DrawMenuScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_STORE:
			DrawStoreScene(&g, theApp, ToWindow(theTile), -theTile.mDest.mX / (theTile.mDest.mWidth / theTile.mSource.mWidth) + theTile.mSource.mX);
			break;
		case PhoneLayout::SURROUND_PAGE:	DrawPageScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_GARDEN:	DrawGardenScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_NOTE:	DrawNoteScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_TREE:	DrawTreeScene(&g, theApp, ToWindow(theTile));	break;
		case PhoneLayout::SURROUND_CREDITS:	DrawCreditsScene(&g, theApp, ToWindow(theTile));	break;
		default:							break;
		}
		break;
	case PhoneLayout::Kind::Band:
		DrawBand(&g, aBoard, theTile);
		break;
	case PhoneLayout::Kind::Title:
		DrawTitle(&g, theApp, theTile);
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
