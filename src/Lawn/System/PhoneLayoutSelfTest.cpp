/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local phone layout patch).
 */

#ifdef PVZP_PHONE_LAYOUT_SELFTEST

#include "PhoneLayoutSelfTest.h"
#include "LawnApp.h"
#include "Lawn/Board.h"
#include "Lawn/Challenge.h"
#include "Lawn/CursorObject.h"
#include "Lawn/Cutscene.h"
#include "Lawn/Plant.h"
#include "Lawn/SeedPacket.h"
#include "Lawn/System/PlayerInfo.h"
#include "Lawn/System/ProfileMgr.h"
#include "Lawn/Widget/GameButton.h"
#include "Lawn/Widget/SeedChooserScreen.h"
#include "Lawn/Widget/TitleScreen.h"
#include "graphics/GLInterface.h"
#include "imagelib/ImageLib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <string>
#include <vector>

using namespace Sexy;

namespace PhoneLayoutSelfTest
{

namespace
{

enum class Step
{
	Start, Loading, MainMenu, Screens, NextLevel, Intro, WaitTiled, RefOff, RefCapture, TiledOn, TiledCapture,
	PickPacket, PlacePlant, PickShovel, Dig, Menu, MenuClosed, Watch, Done
};

struct Level
{
	std::string	mName;
	GameMode	mMode;
	int			mAdventureLevel;	// adventure only
	int			mExtraSlots;		// packets above 6
};

struct State
{
	bool					mInit = false;
	bool					mActive = false;
	Step					mStep = Step::Start;
	int						mTicks = 0;			// in the current step
	int						mTotalTicks = 0;
	int						mScreenWidth = 852, mScreenHeight = 393;
	bool					mHasSafe = false;
	SDL_Rect				mSafe{};
	std::vector<Level>		mLevels;
	size_t					mLevel = 0;
	std::string				mReportPath;
	FILE*					mReport = nullptr;
	int						mChecks = 0, mFailures = 0, mLevelFailures = 0;
	GLInterface::FrameCapture mRef;
	bool					mSuppress = false;
	int						mPacket = -1, mCol = -1, mRow = -1;
	int						mChooserTicks = 0;	// the seed chooser up
	int						mScreen = 0;		// of the screens outside the levels
	bool					mTitleSeen = false;
	Plant*					mPlant = nullptr;
	Plant*					mPlantBefore = nullptr;	// the pot or lily pad it goes on, if any
	bool					mLastTiled = false;
	int						mTransitions = 0, mPlayTicks = 0, mTiledTicks = 0;
	std::map<std::string, int> mOffReasons;
	bool					mShow = false;		// only into the first level, then the player plays
	std::string				mShotDir;			// where the pictures of the window go, one per level; none without
	std::string				mPhone;				// the name of the phone, for the names of the pictures
};

State gState;

template <class... Args>
void Log(std::format_string<Args...> theFormat, Args&&... theArgs)
{
	std::string aLine = std::format(theFormat, std::forward<Args>(theArgs)...);
	std::printf("%s\n", aLine.c_str());
	std::fflush(stdout);
	if (gState.mReport)
	{
		std::fprintf(gState.mReport, "%s\n", aLine.c_str());
		std::fflush(gState.mReport);
	}
}

void Check(bool theOk, const std::string& theWhat)
{
	gState.mChecks++;
	if (!theOk)
	{
		gState.mFailures++;
		gState.mLevelFailures++;
	}
	Log("  [{}] {}", theOk ? "ok" : "FAIL", theWhat);
}

void Goto(Step theStep)
{
	gState.mStep = theStep;
	gState.mTicks = 0;
}

void Finish()
{
	Log("");
	Log("{} checks, {} failed", gState.mChecks, gState.mFailures);
	if (gState.mReport)
		std::fclose(gState.mReport);
	std::fflush(stdout);
	std::_Exit(gState.mFailures == 0 ? 0 : 1);
}

// ten seconds of play from here, counting the switches between tiled and letterboxed
void StartWatch()
{
	gState.mTransitions = 0;
	gState.mStep = Step::Watch;
	gState.mTicks = 0;
}

void FailLevel(const std::string& theWhy)
{
	Check(false, theWhy);
	Goto(Step::NextLevel);
}

// --- pixels -------------------------------------------------------------------------------------------------------

struct Rgb
{
	int r = 0, g = 0, b = 0;
};

Rgb Pixel(const std::vector<unsigned char>& theBuffer, int theWidth, int theHeight, int theCol, int theRow)
{
	theCol = std::clamp(theCol, 0, theWidth - 1);
	theRow = std::clamp(theRow, 0, theHeight - 1);
	size_t i = (static_cast<size_t>(theRow) * theWidth + theCol) * 4;
	return Rgb{ theBuffer[i], theBuffer[i + 1], theBuffer[i + 2] };
}

int Difference(const Rgb& a, const Rgb& b)
{
	return std::max({ std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b) });
}

int Brightness(const Rgb& a)
{
	return std::max({ a.r, a.g, a.b });
}

// the logical screen, from the texture of a tiled frame
Rgb Logical(const GLInterface::FrameCapture& c, double theX, double theY)
{
	return Pixel(c.mLogical, c.mLogicalWidth, c.mLogicalHeight, static_cast<int>(theX * c.mLogicalWidth / 800.0),
		c.mLogicalHeight - 1 - static_cast<int>(theY * c.mLogicalHeight / 600.0));
}

// the logical screen, from the letterbox of a frame drawn without tiles
Rgb Letterboxed(const GLInterface::FrameCapture& c, double theX, double theY)
{
	return Pixel(c.mWindow, c.mWindowWidth, c.mWindowHeight, c.mLetterboxX + static_cast<int>(theX * c.mLetterboxWidth / 800.0),
		c.mLetterboxY + c.mLetterboxHeight - 1 - static_cast<int>(theY * c.mLetterboxHeight / 600.0));
}

// the window, at a point
Rgb Window(const GLInterface::FrameCapture& c, double theX, double theY)
{
	return Pixel(c.mWindow, c.mWindowWidth, c.mWindowHeight, static_cast<int>(theX * c.mWindowWidth / c.mPointWidth),
		c.mWindowHeight - 1 - static_cast<int>(theY * c.mWindowHeight / c.mPointHeight));
}

bool InAnyTile(const std::vector<PresentationTile>& theTiles, double theX, double theY, double theMargin)
{
	for (const PresentationTile& t : theTiles)
	{
		if (theX >= t.mDest.mX - theMargin && theX < t.mDest.mX + t.mDest.mWidth + theMargin &&
			theY >= t.mDest.mY - theMargin && theY < t.mDest.mY + t.mDest.mHeight + theMargin)
			return true;
	}
	return false;
}

void CheckPicture(LawnApp* theApp, const GLInterface::FrameCapture& theRef, const GLInterface::FrameCapture& theTiled)
{
	const std::vector<PresentationTile>& aTiles = theTiled.mTiles;
	Log("  window {}x{} points, {}x{} pixels; texture of the logical screen {}x{}", theTiled.mPointWidth, theTiled.mPointHeight,
		theTiled.mWindowWidth, theTiled.mWindowHeight, theTiled.mLogicalWidth, theTiled.mLogicalHeight);
	for (size_t i = 0; i < aTiles.size(); i++)
	{
		const PresentationTile& t = aTiles[i];
		Log("  tile {}{}: ({:.1f},{:.1f} {:.1f}x{:.1f}) -> window ({:.1f},{:.1f} {:.1f}x{:.1f}), x{:.3f}", i,
			t.mImage ? " (backdrop image)" : t.mOverlay == 2 ? " (card)" : t.mOverlay == 3 ? " (sun counter)" : t.mOverlay == 4 ? " (progress meter)" : t.mOverlay == 5 ? " (menu button)" : t.mOverlay == 6 ? " (shovel)" : t.mOverlay == 7 ? " (conveyor belt)" : t.mOverlay == 8 ? " (weather)" : t.mOverlay == 10 ? " (main menu scenery)" : t.mOverlay == 11 ? " (mirrored background)" : t.mBlur ? " (blurred)" : "",
			t.mSource.mX, t.mSource.mY, t.mSource.mWidth, t.mSource.mHeight, t.mDest.mX, t.mDest.mY, t.mDest.mWidth,
			t.mDest.mHeight, t.mDest.mWidth / t.mSource.mWidth);
	}
	Check(theTiled.mTiled && !aTiles.empty(), "the frame was drawn tiled");
	Check(!theRef.mTiled && theRef.mLetterboxWidth > 0, "the frame to compare with was drawn letterboxed");
	if (!theTiled.mTiled || theRef.mTiled || theRef.mWindow.empty() || theTiled.mLogical.empty())
		return;

	// the stormy night is dark but for its lightning, over the game's screen and (as a tile of weather) over each piece of
	// the backdrop: what it shows is not checked, only looked at
	if (theApp->IsStormyNightLevel())
	{
		size_t aBackdrops = 0, aWeather = 0;
		for (const PresentationTile& t : aTiles)
		{
			aBackdrops += t.mImage != nullptr;
			aWeather += t.mOverlay == 8;
		}
		Log("  a stormy night: {} pieces of the backdrop, {} of weather over them; the picture is not checked", aBackdrops, aWeather);
		Check(aWeather == aBackdrops, "the darkness of the storm is over every piece of the backdrop");
		return;
	}

	// 1. the texture holds the same picture as the letterboxed screen (a few frames apart: some things move), but for
	// what the layout draws into the window instead: the seed bank (all of it around the packets and the sun counter),
	// the shovel, the progress meter, the menu button
	std::vector<FRect> aMoved;
	FRect aBank(10000, 10000, 0, 0);
	for (const PresentationTile& t : aTiles)
	{
		if (t.mOverlay == 0)
			continue;
		aMoved.push_back(FRect(t.mSource.mX - 3, t.mSource.mY - 3, t.mSource.mWidth + 6, t.mSource.mHeight + 6));
		if (t.mOverlay == 7)	// the conveyor belt: where the game has it
			aMoved.push_back(FRect(t.mSource.mX - 8, 0, t.mSource.mWidth + 16, t.mSource.mHeight + 4));
		if (t.mOverlay == 2 || t.mOverlay == 3)
		{
			double aRight = std::max(aBank.mX + aBank.mWidth, t.mSource.mX + t.mSource.mWidth);
			aBank.mX = std::min(aBank.mX, t.mSource.mX);
			aBank.mY = 0;
			aBank.mWidth = aRight - aBank.mX;
			aBank.mHeight = 90;
		}
	}
	if (aBank.mWidth > 0)
		aMoved.push_back(FRect(aBank.mX - 12, 0, aBank.mWidth + 24, aBank.mHeight));
	auto IsMoved = [&](double x, double y)
	{
		for (const FRect& r : aMoved)
			if (x >= r.mX && x < r.mX + r.mWidth && y >= r.mY && y < r.mY + r.mHeight)
				return true;
		return false;
	};
	int aSamples = 0, aDiffer = 0, aDifferFlipped = 0, aDifferMirrored = 0;
	long long aTotal = 0;
	for (double y = 2; y < 600; y += 4)
	{
		for (double x = 2; x < 800; x += 4)
		{
			if (IsMoved(x, y) || IsMoved(x, 600 - y) || IsMoved(800 - x, y))
				continue;
			Rgb aRef = Letterboxed(theRef, x, y);
			int d = Difference(Logical(theTiled, x, y), aRef);
			aSamples++;
			aTotal += d;
			aDiffer += d > 48;
			aDifferFlipped += Difference(Logical(theTiled, x, 600 - y), aRef) > 48;
			aDifferMirrored += Difference(Logical(theTiled, 800 - x, y), aRef) > 48;
		}
	}
	double aFraction = static_cast<double>(aDiffer) / aSamples;
	Log("  texture vs letterboxed screen: {:.1f}% of points differ (upside down {:.1f}%, mirrored {:.1f}%), mean difference {:.1f}",
		aFraction * 100, 100.0 * aDifferFlipped / aSamples, 100.0 * aDifferMirrored / aSamples, static_cast<double>(aTotal) / aSamples);
	Check(aFraction < 0.12, "the texture of the logical screen matches the letterboxed screen");
	Check(aDiffer < aDifferFlipped, "the texture is the right way up");

	// 2. each tile in the window shows its piece of the texture
	for (size_t i = 0; i < aTiles.size(); i++)
	{
		const PresentationTile& t = aTiles[i];
		int aTileSamples = 0, aTileDiffer = 0, aDark = 0;
		const int N = 24;
		for (int iy = 0; iy < N; iy++)
		{
			for (int ix = 0; ix < N; ix++)
			{
				double fx = (ix + 0.5) / N, fy = (iy + 0.5) / N;
				double wx = t.mDest.mX + 1.5 + (t.mDest.mWidth - 3) * fx;
				double wy = t.mDest.mY + 1.5 + (t.mDest.mHeight - 3) * fy;
				double lx = t.mSource.mX + (wx - t.mDest.mX) * t.mSource.mWidth / t.mDest.mWidth;
				double ly = t.mSource.mY + (wy - t.mDest.mY) * t.mSource.mHeight / t.mDest.mHeight;
				// only where this tile is on top: a later one may lie over it (the menu button over the game's screen)
				bool aCovered = false;
				for (size_t j = i + 1; j < aTiles.size() && !aCovered; j++)
					aCovered = InAnyTile(std::vector<PresentationTile>{ aTiles[j] }, wx, wy, 1.5);
				if (aCovered)
					continue;
				Rgb aShown = Window(theTiled, wx, wy);
				aTileSamples++;
				aDark += Brightness(aShown) < 16;
				if (t.mImage != nullptr || t.mOverlay != 0)
					continue;
				// the nearest texel or one beside it: the window is filtered, the texel picked is the nearest
				int aBest = 255;
				double aTexel = 600.0 / theTiled.mLogicalHeight;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++)
						aBest = std::min(aBest, Difference(aShown, Logical(theTiled, lx + dx * aTexel, ly + dy * aTexel)));
				aTileDiffer += aBest > 40;
			}
		}
		double aTileFraction = static_cast<double>(aTileDiffer) / std::max(1, aTileSamples);
		double aDarkFraction = static_cast<double>(aDark) / std::max(1, aTileSamples);
		if (t.mImage != nullptr)
		{
			Log("  tile {} (the backdrop): {:.1f}% black", i, aDarkFraction * 100);
			Check(aDarkFraction < 0.25, std::format("tile {} (the backdrop) is drawn", i));
			continue;
		}
		if (t.mOverlay != 0)
		{
			Log("  tile {} (drawn into the window): {:.1f}% black", i, aDarkFraction * 100);
			Check(aDarkFraction < 0.25, std::format("tile {} (drawn into the window) is drawn", i));
			continue;
		}
		Log("  tile {}: {:.1f}% of points differ from the texture, {:.1f}% black", i, aTileFraction * 100, aDarkFraction * 100);
		Check(aTileFraction < 0.06, std::format("tile {} shows its piece of the logical screen", i));
		Check(aDarkFraction < 0.25, std::format("tile {} is not black", i));
	}

	// 3. black around the tiles, and the window mostly covered
	int aOutside = 0, aOutsideLit = 0, aAll = 0, aAllLit = 0;
	for (double y = 1; y < theTiled.mPointHeight; y += 2)
	{
		for (double x = 1; x < theTiled.mPointWidth; x += 2)
		{
			Rgb p = Window(theTiled, x, y);
			aAll++;
			aAllLit += Brightness(p) > 12;
			if (!InAnyTile(aTiles, x, y, 1.5))
			{
				aOutside++;
				aOutsideLit += Brightness(p) > 12;
			}
		}
	}
	Log("  {:.1f}% of the window is lit; {:.1f}% of the window is outside the tiles, {:.1f}% of that lit",
		100.0 * aAllLit / aAll, 100.0 * aOutside / aAll, aOutside ? 100.0 * aOutsideLit / aOutside : 0.0);
	Check(aOutside == 0 || aOutsideLit * 100 < aOutside, "the window is black outside the tiles");

	// 4. nothing large is black right of the safe area's left edge (left of it is the notch or the Dynamic Island)
	double aSafeLeft = gState.mHasSafe ? gState.mSafe.x : 0;
	int aRightOfSafe = 0, aBlack = 0;
	for (double y = 1; y < theTiled.mPointHeight; y += 2)
	{
		for (double x = aSafeLeft + 1; x < theTiled.mPointWidth; x += 2)
		{
			aRightOfSafe++;
			aBlack += Brightness(Window(theTiled, x, y)) <= 12;
		}
	}
	Log("  {:.2f}% of the window right of the safe area's left edge is black", aRightOfSafe ? 100.0 * aBlack / aRightOfSafe : 0.0);
	Check(aBlack * 100 < aRightOfSafe * 3, "nothing large is black right of the safe area's left edge");
	Check(aAllLit * 100 > aAll * 85, "the game covers most of the window");
	(void)theApp;
}

// the window as captured, as a PNG picture: build-phone/screenshots/<phone>-<level>.png
void SaveWindow(const GLInterface::FrameCapture& c, const std::string& theLevel)
{
	if (gState.mShotDir.empty() || c.mWindow.empty() || c.mWindowWidth <= 0 || c.mWindowHeight <= 0)
		return;
	ImageLib::Image anImage;
	anImage.mWidth = c.mWindowWidth;
	anImage.mHeight = c.mWindowHeight;
	anImage.mBits = std::make_unique<uint32_t[]>(static_cast<size_t>(c.mWindowWidth) * c.mWindowHeight);
	for (int y = 0; y < c.mWindowHeight; y++)
	{
		const unsigned char* aRow = &c.mWindow[static_cast<size_t>(c.mWindowHeight - 1 - y) * c.mWindowWidth * 4];
		uint32_t* anOut = &anImage.mBits[static_cast<size_t>(y) * c.mWindowWidth];
		for (int x = 0; x < c.mWindowWidth; x++)
			anOut[x] = 0xFF000000u | (aRow[x * 4] << 16) | (aRow[x * 4 + 1] << 8) | aRow[x * 4 + 2];
	}
	std::string aName = theLevel;
	std::replace(aName.begin(), aName.end(), ' ', '-');
	std::string aPath = std::format("{}/{}-{}.png", gState.mShotDir, gState.mPhone.empty() ? std::string("phone") : gState.mPhone, aName);
	Log("  picture of the window: {}", ImageLib::WritePNGImage(aPath, &anImage) ? aPath : std::string("could not be written"));
}

// --- framed: outside the level layout ------------------------------------------------------------------------------

// the frame is in the phone layout of a level (and has been for a frame: tiles apply with the next one)
bool InLevelLayout(LawnApp* theApp)
{
	return theApp->mPhonePresentation == LawnApp::PhonePresentation::Level && theApp->mGLInterface->IsTiled();
}

// a frame shown framed: the game's screen as tall as the window, a blurred copy under all, nothing black right of the
// safe area's left edge, and around the screen: the main menu's scenery (with the screen where the level layout has it,
// or in the middle for a page), the backdrop of the level (the screen where the level layout has it), the store's
// background mirrored, or nothing but the blur
enum class Framed { Plain, MenuScene, Backdrop, Page, Mirrored };

void CheckFramed(LawnApp* theApp, const GLInterface::FrameCapture& c, const std::string& theWhat, Framed theExpected)
{
	const PresentationTile* aBlur = nullptr;
	const PresentationTile* aScreen = nullptr;
	int aBackdrops = 0, aScenes = 0, aMirrored = 0;
	for (const PresentationTile& t : c.mTiles)
	{
		if (t.mBlur)
			aBlur = &t;
		else if (t.mImage != nullptr)
			aBackdrops++;
		else if (t.mOverlay == 10)
			aScenes++;
		else if (t.mOverlay == 11)
			aMirrored++;
		else if (t.mOverlay == 0)
			aScreen = &t;
	}
	Check(c.mTiled && aBlur && aScreen, theWhat + " is framed: the game's screen, a blurred copy under all");
	if (!aBlur || !aScreen)
		return;
	double aSafeLeft = gState.mHasSafe ? gState.mSafe.x : 0, aSafeWidth = gState.mHasSafe ? gState.mSafe.w : c.mPointWidth;
	// the whole screen, also where a page's frame is cut off it
	double aScale = aScreen->mDest.mHeight / aScreen->mSource.mHeight;
	double aScreenX = aScreen->mDest.mX - aScreen->mSource.mX * aScale, aScreenWidth = 800 * aScale;
	Log("  {}: the game's screen at ({:.1f},{:.1f} {:.1f}x{:.1f}); {} pieces of the backdrop, {} of scenery, {} mirrored", theWhat, aScreenX,
		aScreen->mDest.mY, aScreenWidth, aScreen->mDest.mHeight, aBackdrops, aScenes, aMirrored);
	Check(std::abs(aScreen->mDest.mHeight - c.mPointHeight) < 1, theWhat + ": the game's screen is as tall as the window");
	Check(aBlur->mDest.mX <= 0 && aBlur->mDest.mY <= 0 && aBlur->mDest.mWidth >= c.mPointWidth && aBlur->mDest.mHeight >= c.mPointHeight,
		theWhat + ": the blurred copy covers the window");
	bool aScenery = theExpected == Framed::MenuScene || theExpected == Framed::Page;
	Check((aScenes > 0) == aScenery, theWhat + (aScenery ? ": the main menu's scenery is around it" : ": no main menu scenery"));
	// (on the pool only the backdrop's part left of the lawn is shown, the image having no water: when the board is panned
	// there may be none of it on the window)
	bool aMayLack = theExpected == Framed::Backdrop && theApp->mBoard && theApp->mBoard->StageHasPool() && theApp->mBoard->mX != 0;
	Check(((aBackdrops > 0) || aMayLack) == (theExpected == Framed::Backdrop), theWhat + (theExpected == Framed::Backdrop ? ": the backdrop of the level is beside it" : ": no backdrop"));
	Check((aMirrored > 0) == (theExpected == Framed::Mirrored), theWhat + (theExpected == Framed::Mirrored ? ": the background is mirrored around it" : ": no mirrored background"));
	if (theExpected == Framed::Backdrop || theExpected == Framed::MenuScene)
		Check(std::abs(aScreenX + aScreenWidth - (aSafeLeft + aSafeWidth)) < 1, theWhat + ": the game's screen is where the level layout has it");
	else
		Check(std::abs(aScreenX + aScreenWidth / 2 - (aSafeLeft + aSafeWidth / 2)) < 1, theWhat + ": the game's screen is in the middle of the safe area");
	if (theApp->IsStormyNightLevel())
		return;	// dark but for its lightning: not checked
	int aSamples = 0, aBlack = 0, aSides = 0, aSidesLit = 0;
	for (double y = 1; y < c.mPointHeight; y += 2)
	{
		for (double x = aSafeLeft + 1; x < c.mPointWidth; x += 2)
		{
			int b = Brightness(Window(c, x, y));
			aSamples++;
			aBlack += b <= 12;
			if (x < aScreenX - 1 || x > aScreenX + aScreenWidth + 1)
			{
				aSides++;
				aSidesLit += b > 12;
			}
		}
	}
	Log("  {}: {:.2f}% black right of the safe area's left edge; beside the game's screen {:.1f}% lit", theWhat, 100.0 * aBlack / std::max(1, aSamples),
		100.0 * aSidesLit / std::max(1, aSides));
	Check(aBlack * 100 < aSamples * 3, theWhat + ": nothing large is black right of the safe area's left edge");
	Check(aSidesLit * 100 > aSides * 90, theWhat + ": beside the game's screen something shows");
}

// --- taps through the tiles ----------------------------------------------------------------------------------------

bool ToWindow(LawnApp* theApp, double theX, double theY, double& theWindowX, double& theWindowY)
{
	const std::vector<PresentationTile>& aTiles = theApp->mGLInterface->GetPresentationTiles();
	for (auto anItr = aTiles.rbegin(); anItr != aTiles.rend(); ++anItr)
	{
		const PresentationTile& t = *anItr;
		if (t.mImage != nullptr || t.mNoTap)
			continue;	// the backdrop (in the pixels of its image), and what takes no tap
		if (theX < t.mSource.mX || theX >= t.mSource.mX + t.mSource.mWidth || theY < t.mSource.mY || theY >= t.mSource.mY + t.mSource.mHeight)
			continue;
		theWindowX = t.mDest.mX + (theX - t.mSource.mX) * t.mDest.mWidth / t.mSource.mWidth;
		theWindowY = t.mDest.mY + (theY - t.mSource.mY) * t.mDest.mHeight / t.mSource.mHeight;
		return true;
	}
	return false;
}

void Tap(LawnApp* theApp, double theX, double theY)
{
	SDL_WindowID anId = SDL_GetWindowID(theApp->mWindow.get());
	SDL_Event aMove{};
	aMove.type = SDL_EVENT_MOUSE_MOTION;
	aMove.motion.windowID = anId;
	aMove.motion.x = static_cast<float>(theX);
	aMove.motion.y = static_cast<float>(theY);
	SDL_PushEvent(&aMove);
	SDL_Event aDown{};
	aDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	aDown.button.windowID = anId;
	aDown.button.button = SDL_BUTTON_LEFT;
	aDown.button.down = true;
	aDown.button.clicks = 1;
	aDown.button.x = static_cast<float>(theX);
	aDown.button.y = static_cast<float>(theY);
	SDL_PushEvent(&aDown);
	SDL_Event anUp = aDown;
	anUp.type = SDL_EVENT_MOUSE_BUTTON_UP;
	anUp.button.down = false;
	SDL_PushEvent(&anUp);
}

// taps a point of the logical screen where the window shows it; false when no tile shows it
bool TapLogical(LawnApp* theApp, double theX, double theY, const std::string& theWhat)
{
	double wx = 0, wy = 0;
	bool aShown = ToWindow(theApp, theX, theY, wx, wy);
	Check(aShown, std::format("{} (logical {:.0f},{:.0f}) is on screen", theWhat, theX, theY));
	if (!aShown)
		return false;
	double aBackX = wx, aBackY = wy;
	theApp->mGLInterface->MapWindowToLogical(aBackX, aBackY);
	Log("  tap {} at window ({:.1f},{:.1f}), which maps back to logical ({:.1f},{:.1f})", theWhat, wx, wy, aBackX, aBackY);
	Tap(theApp, wx, wy);
	return true;
}

std::vector<Level> ParseLevels(const char* theList)
{
	std::vector<Level> aAll = {
		{ "adventure 1-1", GameMode::GAMEMODE_ADVENTURE, 1, 0 },
		{ "adventure 1-3", GameMode::GAMEMODE_ADVENTURE, 3, 0 },
		{ "survival day", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_1, 0, 0 },
		{ "survival day 10 packets", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_1, 0, 4 },
		{ "survival night", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_2, 0, 1 },
		{ "survival pool", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_3, 0, 2 },
		{ "survival fog", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_4, 0, 3 },
		{ "survival roof", GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_5, 0, 0 },
		{ "adventure 1-10", GameMode::GAMEMODE_ADVENTURE, 10, 0 },		// a conveyor belt
		{ "adventure 2-10", GameMode::GAMEMODE_ADVENTURE, 20, 0 },		// a conveyor belt at night
		{ "adventure 4-10", GameMode::GAMEMODE_ADVENTURE, 40, 0 },		// a conveyor belt, in a stormy night: dark but for lightning
	};
	if (theList == nullptr || theList[0] == '\0')
		return aAll;
	std::vector<std::string> aWords;
	std::string aList = theList;
	for (size_t aStart = 0; aStart < aList.size();)
	{
		size_t anEnd = aList.find(' ', aStart);
		if (anEnd == std::string::npos)
			anEnd = aList.size();
		if (anEnd > aStart)
			aWords.push_back(aList.substr(aStart, anEnd - aStart));
		aStart = anEnd + 1;
	}
	if (aWords.empty())
		return aAll;
	std::vector<Level> aLevels;
	for (const Level& aLevel : aAll)
	{
		std::string aKey = aLevel.mName;
		std::replace(aKey.begin(), aKey.end(), ' ', '-');
		if (std::find(aWords.begin(), aWords.end(), aKey) != aWords.end())
			aLevels.push_back(aLevel);
	}
	return aLevels;
}

void Init(LawnApp* theApp)
{
	gState.mInit = true;
	const char* anOn = std::getenv("PVZP_SELFTEST");
	gState.mActive = anOn != nullptr && anOn[0] == '1';
	if (!gState.mActive)
		return;
	if (const char* aScreen = std::getenv("PVZP_SELFTEST_SCREEN"))
		std::sscanf(aScreen, "%dx%d", &gState.mScreenWidth, &gState.mScreenHeight);
	if (const char* aSafe = std::getenv("PVZP_SELFTEST_SAFE"))
		gState.mHasSafe = std::sscanf(aSafe, "%d,%d,%d,%d", &gState.mSafe.x, &gState.mSafe.y, &gState.mSafe.w, &gState.mSafe.h) == 4;
	gState.mLevels = ParseLevels(std::getenv("PVZP_SELFTEST_LEVELS"));
	if (const char* aShow = std::getenv("PVZP_SELFTEST_SHOW"))
		gState.mShow = aShow[0] == '1';
	if (const char* aShots = std::getenv("PVZP_SELFTEST_SHOTS"))
		gState.mShotDir = aShots;
	if (const char* aPhone = std::getenv("PVZP_SELFTEST_PHONE"))
		gState.mPhone = aPhone;
	if (const char* aReport = std::getenv("PVZP_SELFTEST_REPORT"))
	{
		gState.mReportPath = aReport;
		gState.mReport = std::fopen(aReport, "a");
	}
	Log("phone {}x{} points, safe area {}", gState.mScreenWidth, gState.mScreenHeight,
		gState.mHasSafe ? std::format("{},{} {}x{}", gState.mSafe.x, gState.mSafe.y, gState.mSafe.w, gState.mSafe.h) : std::string("the whole window"));
	theApp->SetMusicVolume(0);
	theApp->SetSfxVolume(0);
	SDL_SetWindowFullscreen(theApp->mWindow.get(), false);
	SDL_SetWindowSize(theApp->mWindow.get(), gState.mScreenWidth, gState.mScreenHeight);
	SDL_SyncWindow(theApp->mWindow.get());
}

void SetUpProfile(LawnApp* theApp)
{
	while (theApp->GetDialogCount() > 0 && !theApp->mDialogMap.empty())
		theApp->KillDialog(theApp->mDialogMap.begin()->first);
	if (theApp->mPlayerInfo == nullptr)
	{
		theApp->mPlayerInfo = theApp->mProfileMgr->AddProfile("PhoneTest");
		if (theApp->mPlayerInfo == nullptr)
			theApp->mPlayerInfo = theApp->mProfileMgr->GetProfile("PhoneTest");
	}
}

void StartLevel(LawnApp* theApp, const Level& theLevel)
{
	PlayerInfo* aPlayer = theApp->mPlayerInfo;
	if (theLevel.mMode == GameMode::GAMEMODE_ADVENTURE)
	{
		aPlayer->mLevel = theLevel.mAdventureLevel;
		aPlayer->mFinishedAdventure = 0;
	}
	else
	{
		aPlayer->mLevel = 50;
		aPlayer->mFinishedAdventure = 1;
	}
	aPlayer->mPurchases[StoreItem::STORE_ITEM_PACKET_UPGRADE] = theLevel.mExtraSlots;
	while (theApp->GetDialogCount() > 0 && !theApp->mDialogMap.empty())
		theApp->KillDialog(theApp->mDialogMap.begin()->first);
	theApp->KillGameSelector();
	theApp->KillBoard();
	theApp->PreNewGame(theLevel.mMode, false);
}

void WatchLayout(LawnApp* theApp)
{
	if (theApp->mGameScene != GameScenes::SCENE_PLAYING)
		return;
	bool aTiled = InLevelLayout(theApp);
	gState.mPlayTicks++;
	gState.mTiledTicks += aTiled;
	if (aTiled != gState.mLastTiled)
		gState.mTransitions++;
	gState.mLastTiled = aTiled;
	if (!aTiled)
		gState.mOffReasons[gState.mSuppress ? std::string("letterboxed by the test") : std::string(theApp->mPhoneLayoutOff)]++;
}

}

void SafeArea(SDL_Rect& theSafeArea)
{
	if (gState.mActive && gState.mHasSafe)
		theSafeArea = gState.mSafe;
}

bool SuppressTiles()
{
	return gState.mActive && gState.mSuppress;
}

void Update(LawnApp* theApp)
{
	if (!gState.mInit)
		Init(theApp);
	if (!gState.mActive)
		return;
	gState.mTicks++;
	gState.mTotalTicks++;
	if (!gState.mShow && gState.mTotalTicks > 100 * 600)
	{
		Check(false, "the test ended within ten minutes");
		Finish();
	}
	WatchLayout(theApp);
	Board* aBoard = theApp->mBoard;
	if (aBoard && aBoard->mChallenge && theApp->IsStormyNightLevel() && theApp->mGameScene == GameScenes::SCENE_PLAYING)
	{
		// a flash of lightning held, for something to see in the pictures
		aBoard->mChallenge->mChallengeState = ChallengeState::STATECHALLENGE_STORM_FLASH_3;
		aBoard->mChallenge->mChallengeStateCounter = 75;
	}
	GLInterface* aGL = theApp->mGLInterface.get();

	switch (gState.mStep)
	{
	case Step::Start:
		Goto(Step::Loading);
		break;

	case Step::Loading:
		if (!theApp->mLoadingThreadCompleted)
			break;
		if (theApp->mTitleScreen && !gState.mShow && !gState.mTitleSeen)
		{
			// the title screen (after the logos, a while into it), framed, before going on
			if (theApp->mTitleScreen->mTitleState != TitleState::TITLESTATE_SCREEN || theApp->mTitleScreen->mTitleStateCounter > 0 && gState.mTicks < 3000 &&
				theApp->mTitleScreen->mTitleAge < 200)
				break;
			if (!aGL->mCapture.mRequested && !aGL->mCapture.mDone)
				aGL->mCapture.mRequested = true;
			if (!aGL->mCapture.mDone)
				break;
			Log("");
			Log("title screen");
			CheckFramed(theApp, aGL->mCapture, "the title screen", Framed::Plain);
			SaveWindow(aGL->mCapture, "title");
			aGL->mCapture = GLInterface::FrameCapture{};
			gState.mTitleSeen = true;
			Goto(Step::Loading);
		}
		if (theApp->mTitleScreen)
			theApp->LoadingCompleted();
		if (gState.mTicks < 50)
			break;
		{
			int w = 0, h = 0, pw = 0, ph = 0;
			SDL_GetWindowSize(theApp->mWindow.get(), &w, &h);
			SDL_GetWindowSizeInPixels(theApp->mWindow.get(), &pw, &ph);
			Log("window {}x{} points, {}x{} pixels", w, h, pw, ph);
			Check(w == gState.mScreenWidth && h == gState.mScreenHeight, "the window has the size of the phone");
		}
		SetUpProfile(theApp);
		Check(theApp->mPlayerInfo != nullptr, "a profile to play with");
		if (theApp->mPlayerInfo == nullptr)
			Finish();
		Goto(gState.mShow ? Step::NextLevel : Step::MainMenu);
		break;

	case Step::MainMenu:
		// the main menu, framed
		if (gState.mTicks == 1)
		{
			if (!theApp->mGameSelector)
				theApp->ShowGameSelector();
			Log("");
			Log("main menu");
		}
		else if (gState.mTicks == 200)
			aGL->mCapture.mRequested = true;
		else if (gState.mTicks > 200 && aGL->mCapture.mDone)
		{
			CheckFramed(theApp, aGL->mCapture, "the main menu", Framed::MenuScene);
			SaveWindow(aGL->mCapture, "main menu");
			aGL->mCapture = GLInterface::FrameCapture{};
			gState.mScreen = 0;
			Goto(Step::Screens);
		}
		else if (gState.mTicks > 1000)
		{
			Check(false, "the main menu was captured");
			Goto(Step::NextLevel);
		}
		break;

	case Step::Screens:
	{
		// other screens outside the levels, framed: the almanac, the store, the mini-games
		static const char* const NAMES[] = { "almanac", "store", "mini-games" };
		if (gState.mScreen >= 3)
		{
			Goto(Step::NextLevel);
			break;
		}
		if (gState.mTicks == 1)
		{
			Log("");
			Log("{}", NAMES[gState.mScreen]);
			if (gState.mScreen == 0)
				theApp->DoAlmanacDialog();
			else if (gState.mScreen == 1)
				theApp->ShowStoreScreen();
			else
			{
				theApp->KillGameSelector();	// as the main menu does when the mini-games are picked
				theApp->ShowChallengeScreen(ChallengePage::CHALLENGE_PAGE_CHALLENGE);
			}
		}
		else if (gState.mTicks == 200)
		{
			aGL->mCapture = GLInterface::FrameCapture{};
			aGL->mCapture.mRequested = true;
		}
		else if (gState.mTicks > 200 && (aGL->mCapture.mDone || gState.mTicks > 1000))
		{
			if (aGL->mCapture.mDone)
			{
				CheckFramed(theApp, aGL->mCapture, std::format("the {}", NAMES[gState.mScreen]), gState.mScreen == 1 ? Framed::Mirrored : Framed::Page);
				SaveWindow(aGL->mCapture, NAMES[gState.mScreen]);
			}
			else
				Check(false, std::format("the {} was captured", NAMES[gState.mScreen]));
			aGL->mCapture = GLInterface::FrameCapture{};
			if (gState.mScreen == 0)
				theApp->KillAlmanacDialog();
			else if (gState.mScreen == 1)
				theApp->KillStoreScreen();
			else
			{
				theApp->KillChallengeScreen();
				theApp->ShowGameSelector();
			}
			gState.mScreen++;
			Goto(Step::Screens);
		}
		break;
	}

	case Step::NextLevel:
		gFastMo = false;
		gState.mSuppress = false;
		if (gState.mLevel > 0)
		{
			Log("  in play {:.1f} s, tiled {:.1f} s", gState.mPlayTicks / 100.0, gState.mTiledTicks / 100.0);
			for (const auto& [aWhy, aTicks] : gState.mOffReasons)
				Log("  letterboxed {:.1f} s: {}", aTicks / 100.0, aWhy.empty() ? "(no reason)" : aWhy);
			Log("  {}", gState.mLevelFailures == 0 ? "level passed" : std::format("level FAILED ({} checks)", gState.mLevelFailures));
		}
		if (gState.mLevel >= gState.mLevels.size())
		{
			Goto(Step::Done);
			break;
		}
		gState.mLevelFailures = 0;
		gState.mTransitions = gState.mPlayTicks = gState.mTiledTicks = 0;
		gState.mOffReasons.clear();
		gState.mLastTiled = false;
		gState.mPlant = nullptr;
		gState.mChooserTicks = 0;
		Log("");
		Log("{}", gState.mLevels[gState.mLevel].mName);
		StartLevel(theApp, gState.mLevels[gState.mLevel]);
		gState.mLevel++;
		Goto(Step::Intro);
		break;

	case Step::Intro:
		gFastMo = true;
		while (theApp->GetDialogCount() > 0 && !theApp->mDialogMap.empty())
			theApp->KillDialog(theApp->mDialogMap.begin()->first);
		if (gState.mShow && aBoard && aBoard->mCutScene && aBoard->mCutScene->mSeedChoosing)
		{
			gFastMo = false;	// the player picks the seeds, and plays from here
			gState.mStep = Step::Done;
			break;
		}
		if (theApp->mSeedChooserScreen && aBoard && aBoard->mCutScene && aBoard->mCutScene->mSeedChoosing)
		{
			// the seed chooser, framed: a picture of it once it is up, then seeds picked at random
			gState.mChooserTicks++;
			if (gState.mChooserTicks == 150)
			{
				gFastMo = false;	// a frame drawn for the picture before going on
				aGL->mCapture = GLInterface::FrameCapture{};
				aGL->mCapture.mRequested = true;
			}
			if (gState.mChooserTicks <= 150 || (!aGL->mCapture.mDone && gState.mChooserTicks < 1000))
				break;
			gFastMo = true;
			if (aGL->mCapture.mDone)
			{
				CheckFramed(theApp, aGL->mCapture, "the seed chooser", Framed::Backdrop);
				SaveWindow(aGL->mCapture, gState.mLevels[gState.mLevel - 1].mName + " seed chooser");
				aGL->mCapture = GLInterface::FrameCapture{};
			}
			theApp->mSeedChooserScreen->PickRandomSeeds();
		}
		if (aBoard && aBoard->mCutScene && aBoard->mCutScene->IsShowingCrazyDave() && gState.mTicks % 30 == 0)
			aBoard->mCutScene->AdvanceCrazyDaveDialog(false);	// Crazy Dave talks before some levels: on to the next line
		if (theApp->mGameScene == GameScenes::SCENE_PLAYING)
		{
			gFastMo = false;
			if (gState.mShow)
			{
				gState.mStep = Step::Done;
				break;
			}
			Goto(Step::WaitTiled);
		}
		else if (gState.mTicks > 100 * 60)
			FailLevel(std::format("the level started within a minute (scene {}, {} dialogs, cutscene {})", static_cast<int>(theApp->mGameScene),
				theApp->GetDialogCount(), aBoard && aBoard->mCutScene ? "yes" : "no"));
		break;

	case Step::WaitTiled:
		if (InLevelLayout(theApp) && gState.mTicks > 300)
		{
			Goto(Step::RefOff);
			break;
		}
		if (gState.mTicks > 100 * 15)
			FailLevel(std::format("the level was tiled within 15 s of play (letterboxed because: {})", theApp->mPhoneLayoutOff));
		break;

	case Step::RefOff:
		gState.mSuppress = true;
		if (!aGL->IsTiled() && gState.mTicks > 3)
		{
			aGL->mCapture = GLInterface::FrameCapture{};
			aGL->mCapture.mRequested = true;
			Goto(Step::RefCapture);
		}
		break;

	case Step::RefCapture:
		if (aGL->mCapture.mDone)
		{
			gState.mRef = std::move(aGL->mCapture);
			aGL->mCapture = GLInterface::FrameCapture{};
			gState.mSuppress = false;
			Goto(Step::TiledOn);
		}
		break;

	case Step::TiledOn:
		if (InLevelLayout(theApp) && gState.mTicks > 3)
		{
			aGL->mCapture.mRequested = true;
			Goto(Step::TiledCapture);
		}
		else if (gState.mTicks > 300)
			FailLevel("the tiles came back after a letterboxed frame");
		break;

	case Step::TiledCapture:
		if (aGL->mCapture.mDone)
		{
			GLInterface::FrameCapture aTiled = std::move(aGL->mCapture);
			aGL->mCapture = GLInterface::FrameCapture{};
			CheckPicture(theApp, gState.mRef, aTiled);
			SaveWindow(aTiled, gState.mLevels[gState.mLevel - 1].mName);
			gState.mRef = GLInterface::FrameCapture{};
			Goto(Step::PickPacket);
		}
		break;

	case Step::PickPacket:
	{
		if (!InLevelLayout(theApp))
			break;
		aBoard->mSunMoney = 9990;
		SeedBank& aBank = *aBoard->mSeedBank;
		if (aBoard->HasConveyorBeltSeedBank() && aBank.GetNumSeedsOnConveyorBelt() == 0)
		{
			aBank.AddSeed(SeedType::SEED_PEASHOOTER, true);	// none has come yet: one at the start of the belt
			Goto(Step::PickPacket);
			break;
		}
		if (aBoard->HasConveyorBeltSeedBank() && aBank.mSeedPackets[0].mOffsetX > 0)
		{
			// the belt on until the first packet is at its start (one coming in past the belt's end cannot be taken)
			int aShift = aBank.mSeedPackets[0].mOffsetX;
			for (int i = 0; i < aBank.mNumPackets; i++)
				aBank.mSeedPackets[i].mOffsetX = std::max(0, aBank.mSeedPackets[i].mOffsetX - aShift);
			Goto(Step::PickPacket);
			break;
		}
		if (gState.mTicks < 20)
			break;	// the layout has the cards of the packets as they are now
		gState.mPacket = -1;
		for (int i = 0; i < aBank.mNumPackets && gState.mPacket < 0; i++)
		{
			SeedType aType = aBank.mSeedPackets[i].mPacketType;
			if (aType == SeedType::SEED_NONE || aType == SeedType::SEED_IMITATER || aType == SeedType::SEED_CHERRYBOMB ||
				aType == SeedType::SEED_JALAPENO || aType == SeedType::SEED_SQUASH || aType == SeedType::SEED_DOOMSHROOM ||
				aType == SeedType::SEED_ICESHROOM || aType == SeedType::SEED_BLOVER || aType == SeedType::SEED_INSTANT_COFFEE ||
				aType == SeedType::SEED_GRAVEBUSTER)	// gone when used, or (the grave buster) not to be dug up
				continue;
			for (int aCol : { 4, 3, 5, 2, 6, 1, 7, 0, 8 })
			{
				for (int aRow : { 2, 1, 3, 0, 4, 5 })
				{
					// a free place: nothing there, or a pot or a lily pad to plant on
					if (gState.mPacket < 0 && aBoard->CanPlantAt(aCol, aRow, aType) == PlantingReason::PLANTING_OK &&
						aBoard->GetTopPlantAt(aCol, aRow, PlantPriority::TOPPLANT_ONLY_NORMAL_POSITION) == nullptr)
					{
						gState.mPacket = i;
						gState.mCol = aCol;
						gState.mRow = aRow;
					}
				}
			}
		}
		if (gState.mPacket < 0 && aBoard->StageHasRoof() && gState.mTicks < 25)
		{
			// the roof takes plants in pots: some in the middle, and look again next time
			for (int aRow = 1; aRow <= 3; aRow++)
				for (int aCol = 3; aCol <= 6; aCol++)
					if (aBoard->GetTopPlantAt(aCol, aRow, PlantPriority::TOPPLANT_ANY) == nullptr)
						aBoard->AddPlant(aCol, aRow, SeedType::SEED_FLOWERPOT, SeedType::SEED_NONE);
			break;
		}
		if (gState.mPacket < 0 && gState.mTicks < 40 && aBank.mNumPackets > 0 &&
			aBank.mSeedPackets[0].mPacketType != SeedType::SEED_PEASHOOTER)
		{
			aBank.mSeedPackets[0].SetPacketType(SeedType::SEED_PEASHOOTER);	// none of the seeds picked at random fits
			break;
		}
		Check(gState.mPacket >= 0, "a packet with a place to plant it");
		if (gState.mPacket < 0)
		{
			Goto(Step::Menu);
			break;
		}
		gState.mPlantBefore = aBoard->GetTopPlantAt(gState.mCol, gState.mRow, PlantPriority::TOPPLANT_ANY);
		SeedPacket& aPacket = aBank.mSeedPackets[gState.mPacket];
		aPacket.mActive = true;
		aPacket.mRefreshing = false;
		Log("  packet {} ({} of {}), to plant at column {} row {}", gState.mPacket, static_cast<int>(aPacket.mPacketType), aBank.mNumPackets,
			gState.mCol, gState.mRow);
		TapLogical(theApp, aBank.mX + aPacket.mX + aPacket.mOffsetX + aPacket.mWidth / 2.0, aBank.mY + aPacket.mY + aPacket.mHeight / 2.0, "the packet");
		Goto(Step::PlacePlant);
		break;
	}

	case Step::PlacePlant:
		if (gState.mTicks == 20)
		{
			const SeedPacket& aPacket = aBoard->mSeedBank->mSeedPackets[gState.mPacket];
			Check(aBoard->mCursorObject->mCursorType == CursorType::CURSOR_TYPE_PLANT_FROM_BANK && aBoard->mCursorObject->mType == aPacket.mPacketType,
				std::format("tapping the packet picked it up (cursor {})", static_cast<int>(aBoard->mCursorObject->mCursorType)));
			TapLogical(theApp, aBoard->GridToPixelX(gState.mCol, gState.mRow) + 40, aBoard->GridToPixelY(gState.mCol, gState.mRow) + 40, "the lawn");
		}
		else if (gState.mTicks == 40)
		{
			gState.mPlant = aBoard->GetTopPlantAt(gState.mCol, gState.mRow, PlantPriority::TOPPLANT_ANY);
			if (gState.mPlant == gState.mPlantBefore)
				gState.mPlant = nullptr;
			Check(gState.mPlant != nullptr, "tapping the lawn planted it in the cell tapped");
			if (gState.mPlant == nullptr)
			{
				for (int aRow = 0; aRow < 6; aRow++)
					for (int aCol = 0; aCol < 9; aCol++)
						if (aBoard->GetTopPlantAt(aCol, aRow, PlantPriority::TOPPLANT_ANY) && (aCol != gState.mCol || aRow != gState.mRow))
							Log("  a plant is at column {} row {}", aCol, aRow);
				aBoard->ClearCursor();
			}
			Goto(aBoard->mShowShovel && gState.mPlant ? Step::PickShovel : Step::Menu);
		}
		break;

	case Step::PickShovel:
		if (gState.mTicks == 1)
		{
			Rect aShovel = aBoard->GetShovelButtonRect();
			TapLogical(theApp, aShovel.mX + aShovel.mWidth / 2.0, aShovel.mY + aShovel.mHeight / 2.0, "the shovel");
		}
		else if (gState.mTicks == 20)
		{
			Check(aBoard->mCursorObject->mCursorType == CursorType::CURSOR_TYPE_SHOVEL, "tapping the shovel picked it up");
			TapLogical(theApp, aBoard->GridToPixelX(gState.mCol, gState.mRow) + 40, aBoard->GridToPixelY(gState.mCol, gState.mRow) + 40, "the plant to dig");
			Goto(Step::Dig);
		}
		break;

	case Step::Dig:
		if (gState.mTicks == 20)
		{
			Check(gState.mPlant->mDead || aBoard->GetTopPlantAt(gState.mCol, gState.mRow, PlantPriority::TOPPLANT_ANY) != gState.mPlant,
				"the shovel dug up the plant");
			aBoard->ClearCursor();
			Goto(Step::Menu);
		}
		break;

	case Step::Menu:
		if (gState.mTicks == 1)
		{
			if (!aBoard->mMenuButton || aBoard->mMenuButton->mBtnNoDraw)
			{
				StartWatch();
				break;
			}
			const GameButton& aMenu = *aBoard->mMenuButton;
			TapLogical(theApp, aMenu.mX + aMenu.mWidth / 2.0, std::max(0, aMenu.mY) + (aMenu.mY + aMenu.mHeight - std::max(0, aMenu.mY)) / 2.0, "the menu button");
		}
		else if (gState.mTicks == 30)
		{
			Check(theApp->GetDialogCount() > 0, "tapping the menu button opened the menu");
			Check(theApp->mPhonePresentation == LawnApp::PhonePresentation::Framed, "the menu is shown framed, out of the level layout");
			aGL->mCapture.mRequested = true;
		}
		else if (gState.mTicks > 30 && (aGL->mCapture.mDone || gState.mTicks > 300))
		{
			if (aGL->mCapture.mDone)
			{
				CheckFramed(theApp, aGL->mCapture, "the menu", Framed::Backdrop);
				SaveWindow(aGL->mCapture, gState.mLevels[gState.mLevel - 1].mName + " menu");
			}
			aGL->mCapture = GLInterface::FrameCapture{};
			theApp->KillNewOptionsDialog();
			while (theApp->GetDialogCount() > 0 && !theApp->mDialogMap.empty())
				theApp->KillDialog(theApp->mDialogMap.begin()->first);
			Goto(Step::MenuClosed);
		}
		break;

	case Step::MenuClosed:
		if (InLevelLayout(theApp) && gState.mTicks > 3)
			StartWatch();
		else if (gState.mTicks > 200)
		{
			Check(false, std::format("tiled again after the menu (letterboxed because: {})", theApp->mPhoneLayoutOff));
			StartWatch();
		}
		break;

	case Step::Watch:
		// ten seconds of play, for the switches between tiled and letterboxed; near their end a second picture, with
		// the progress meter shown (it shows from the first wave on) and the packet planted recharging
		if (gState.mTicks == 900 && aBoard)
			aBoard->mProgressMeterWidth = std::max(aBoard->mProgressMeterWidth, 60);
		else if (gState.mTicks == 950 && !gState.mShotDir.empty())
			aGL->mCapture.mRequested = true;
		else if (gState.mTicks > 950 && aGL->mCapture.mDone)
		{
			SaveWindow(aGL->mCapture, gState.mLevels[gState.mLevel - 1].mName + " later");
			aGL->mCapture = GLInterface::FrameCapture{};
		}
		if (gState.mTicks > 1000)
		{
			Check(gState.mTransitions == 0, std::format("the layout stays tiled through ten seconds of play ({} switches)", gState.mTransitions));
			Goto(Step::NextLevel);
		}
		break;

	case Step::Done:
		if (!gState.mShow)
			Finish();
		break;
	}
}

}

#endif
