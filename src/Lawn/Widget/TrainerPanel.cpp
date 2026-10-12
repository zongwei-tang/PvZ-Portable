/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local trainer patch).
 */

#include "TrainerPanel.h"
#include "GameSelector.h"
#include "../Board.h"
#include "../Plant.h"
#include "../Zombie.h"
#include "../System/PlayerInfo.h"
#include "../System/Trainer.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../GameConstants.h"
#include "../../PvzpLib/PvzpStringFile.h"
#include "graphics/Graphics.h"
#include "graphics/Font.h"
#include "widget/Dialog.h"
#include "widget/WidgetManager.h"
#include <algorithm>
#include <format>

using namespace Sexy;

namespace
{
constexpr int ROW_HEIGHT = 30;
constexpr int ITEM_HEIGHT = 26;
constexpr int ARROW_WIDTH = 24;
constexpr int GAME_SPEEDS[] = { 1, 2, 3, 5, 10 };
constexpr int MAX_COINS = 99999; // PlayerInfo::AddCoins clamps to this as well

int Wrap(int theValue, int theCount)
{
	return ((theValue % theCount) + theCount) % theCount;
}

void DrawArrow(Graphics* g, const Rect& theRect, bool theLeft)
{
	int aCenterX = theRect.mX + theRect.mWidth / 2;
	int aCenterY = theRect.mY + theRect.mHeight / 2;
	for (int i = 0; i < 7; i++)
	{
		int aX = theLeft ? aCenterX - 3 + i : aCenterX + 3 - i;
		g->FillRect(aX, aCenterY - 6 + i, 1, 13 - i * 2);
	}
}

// Pointer comparison only, so a widget deleted while the panel was open is never touched
bool TreeHasWidget(const WidgetContainer* theContainer, const Widget* theWidget)
{
	for (Widget* aChild : theContainer->mWidgets)
	{
		if (aChild == theWidget || TreeHasWidget(aChild, theWidget))
			return true;
	}
	return false;
}
}

TrainerPanel::TrainerPanel(LawnApp* theApp) :
	mApp(theApp),
	mOpen(false),
	mPausedBoard(false),
	mPrevFocus(nullptr),
	mHoverItem(-1),
	mPlantType(SeedType::SEED_PEASHOOTER),
	mPlantCol(0),
	mPlantRow(0),
	mZombieIndex(0),
	mZombieRow(-1),
	mZombieCol(-1),
	mStage(0),
	mSlot(0)
{
	mZOrder = 1000000; // above the board and every dialog
	mHasAlpha = true;
	mHasTransparencies = true;
	mVisible = false;
	mMouseVisible = false;

	for (int i = 0; i < ZombieType::NUM_ZOMBIE_TYPES; i++)
	{
		if (i != ZombieType::ZOMBIE_BOSS)
			mZombieTypes.push_back(i);
	}

	BuildItems();
}

void TrainerPanel::AddSwitch(int theX, int theY, std::string theLabel, bool* theValue)
{
	Item anItem{};
	anItem.mRect = Rect(theX, theY, 340, ITEM_HEIGHT);
	anItem.mLabel = [theLabel]() { return theLabel; };
	anItem.mAction = [this, theValue]()
	{
		*theValue = !*theValue;
		mStatus.clear();
	};
	anItem.mChecked = theValue;
	mItems.push_back(std::move(anItem));
}

void TrainerPanel::AddButton(int theX, int theY, int theWidth, std::string theLabel, std::function<void()> theAction)
{
	Item anItem{};
	anItem.mRect = Rect(theX, theY, theWidth, ITEM_HEIGHT);
	anItem.mLabel = [theLabel]() { return theLabel; };
	anItem.mAction = std::move(theAction);
	mItems.push_back(std::move(anItem));
}

void TrainerPanel::AddSelector(int theX, int theY, int theWidth, std::function<std::string()> theLabel, std::function<void(int)> theStep)
{
	Item aLeft{};
	aLeft.mRect = Rect(theX, theY, ARROW_WIDTH, ITEM_HEIGHT);
	aLeft.mAction = [theStep]() { theStep(-1); };
	aLeft.mArrowLeft = true;
	mItems.push_back(std::move(aLeft));

	Item aMiddle{};
	aMiddle.mRect = Rect(theX + ARROW_WIDTH + 2, theY, theWidth - ARROW_WIDTH * 2 - 4, ITEM_HEIGHT);
	aMiddle.mLabel = std::move(theLabel);
	mItems.push_back(std::move(aMiddle));

	Item aRight{};
	aRight.mRect = Rect(theX + theWidth - ARROW_WIDTH, theY, ARROW_WIDTH, ITEM_HEIGHT);
	aRight.mAction = [theStep]() { theStep(1); };
	aRight.mArrowRight = true;
	mItems.push_back(std::move(aRight));
}

void TrainerPanel::BuildItems()
{
	int aY = 95;
	AddSwitch(40, aY, "Infinite Sun", &gTrainer.mInfiniteSun); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "No Cooldown", &gTrainer.mNoCooldown); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Free Planting", &gTrainer.mFreePlanting); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Plant Anywhere", &gTrainer.mPlantAnywhere); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Plants Never Lose Health", &gTrainer.mInvinciblePlants); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Freeze Zombies", &gTrainer.mFreezeZombies); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Stop Zombie Waves", &gTrainer.mStopZombieWaves); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "Auto Collect Sun and Coins", &gTrainer.mAutoCollect); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "All Plants Unlocked", &gTrainer.mAllPlants); aY += ROW_HEIGHT;
	AddSwitch(40, aY, "10 Seed Slots (next level)", &gTrainer.mTenSeedSlots);

	aY = 95;
	AddButton(420, aY, 340, "Add 1000 Sun", [this]() { DoAddSun(); }); aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Kill All Zombies", [this]() { DoKillAllZombies(); }); aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Next Wave Now", [this]() { DoNextWave(); }); aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Win This Level", [this]() { DoWinLevel(); }); aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Add 10000 Coins", [this]() { DoAddCoins(1000); }); aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Max Coins", [this]() { DoMaxCoins(); }); aY += ROW_HEIGHT;
	AddSelector(420, aY, 340,
		[]() { return std::format("Game Speed  {}x", gTrainer.mGameSpeed); },
		[](int theDir)
		{
			constexpr int aCount = static_cast<int>(std::size(GAME_SPEEDS));
			int anIndex = static_cast<int>(std::find(std::begin(GAME_SPEEDS), std::end(GAME_SPEEDS), gTrainer.mGameSpeed) - std::begin(GAME_SPEEDS));
			gTrainer.mGameSpeed = GAME_SPEEDS[Wrap(anIndex + theDir, aCount)];
		});
	aY += ROW_HEIGHT;
	AddButton(420, aY, 340, "Clear Lawn", [this]() { DoClearLawn(); });

	aY = 358;
	AddSelector(420, aY, 170,
		[this]() { return std::string(TrainerStageName(mStage)); },
		[this](int theDir) { mStage = Wrap(mStage + theDir, TRAINER_NUM_STAGES); });
	AddButton(600, aY, 160, "Start Blank Level", [this]() { DoStartBlankLevel(); });
	aY += ROW_HEIGHT;
	AddSelector(420, aY, 150,
		[this]() { return std::format("Slot {}", mSlot + 1); },
		[this](int theDir) { mSlot = Wrap(mSlot + theDir, TRAINER_NUM_SLOTS); });
	AddButton(580, aY, 85, "Save", [this]() { DoSaveLevel(); });
	AddButton(675, aY, 85, "Load", [this]() { DoLoadLevel(); });

	aY = 435;
	AddSelector(40, aY, 260,
		[this]() { return Plant::GetNameString(static_cast<SeedType>(mPlantType)); },
		[this](int theDir) { mPlantType = Wrap(mPlantType + theDir, SeedType::SEED_IMITATER); });
	AddSelector(310, aY, 110,
		[this]() { return std::format("Col {}", mPlantCol + 1); },
		[this](int theDir) { mPlantCol = Wrap(mPlantCol + theDir, MAX_GRID_SIZE_X); });
	AddSelector(430, aY, 110,
		[this]() { return std::format("Row {}", mPlantRow + 1); },
		[this](int theDir) { mPlantRow = Wrap(mPlantRow + theDir, MAX_GRID_SIZE_Y); });
	AddButton(550, aY, 65, "Plant", [this]() { DoSpawnPlant(false); });
	AddButton(625, aY, 65, "Fill", [this]() { DoSpawnPlant(true); });
	AddButton(700, aY, 60, "Mouse", [this]() { DoStartBrush(TRAINER_BRUSH_PLANT); });

	aY = 495;
	AddSelector(40, aY, 260,
		[this]()
		{
			const ZombieDefinition& aDef = GetZombieDefinition(static_cast<ZombieType>(mZombieTypes[mZombieIndex]));
			return std::string(PvzpStringTranslate(std::format("[{}]", aDef.mZombieName)));
		},
		[this](int theDir) { mZombieIndex = Wrap(mZombieIndex + theDir, static_cast<int>(mZombieTypes.size())); });
	AddSelector(310, aY, 110,
		[this]() { return mZombieCol < 0 ? std::string("Col Edge") : std::format("Col {}", mZombieCol + 1); },
		[this](int theDir) { mZombieCol = Wrap(mZombieCol + 1 + theDir, MAX_GRID_SIZE_X + 1) - 1; });
	AddSelector(430, aY, 110,
		[this]() { return mZombieRow < 0 ? std::string("Row Rand") : std::format("Row {}", mZombieRow + 1); },
		[this](int theDir) { mZombieRow = Wrap(mZombieRow + 1 + theDir, MAX_GRID_SIZE_Y + 1) - 1; });
	AddButton(550, aY, 65, "Send", [this]() { DoSpawnZombie(); });
	AddButton(700, aY, 60, "Mouse", [this]() { DoStartBrush(TRAINER_BRUSH_ZOMBIE); });

	AddButton(660, 550, 100, "Close", [this]() { Close(); });
}

void TrainerPanel::Toggle()
{
	if (mOpen)
		Close();
	else
		Open();
}

void TrainerPanel::Open()
{
	if (mOpen)
		return;

	mOpen = true;
	mVisible = true;
	mMouseVisible = true;
	mHoverItem = -1;
	gTrainer.mBrush = TRAINER_BRUSH_NONE;
	Resize(0, 0, mApp->mWidth, mApp->mHeight);

	if (mApp->mBoard && !mApp->mBoard->mPaused)
	{
		mApp->mBoard->Pause(true);
		mPausedBoard = true;
	}

	if (mWidgetManager)
	{
		mPrevFocus = mWidgetManager->mFocusWidget;
		mWidgetManager->SetFocus(this);
	}
	MarkDirty();
}

void TrainerPanel::Close()
{
	if (!mOpen)
		return;

	mOpen = false;
	if (mPausedBoard && mApp->mBoard)
		mApp->mBoard->Pause(false);
	mPausedBoard = false;

	if (mWidgetManager)
	{
		if (mPrevFocus && mPrevFocus != this && TreeHasWidget(mWidgetManager, mPrevFocus))
			mWidgetManager->SetFocus(mPrevFocus);
		else if (!mApp->mDialogList.empty())
			mWidgetManager->SetFocus(mApp->mDialogList.back());
		else if (mApp->mBoard)
			mWidgetManager->SetFocus(mApp->mBoard);
		else if (mApp->mGameSelector)
			mWidgetManager->SetFocus(mApp->mGameSelector.get());
	}
	mPrevFocus = nullptr;

	UpdateClosedState();
}

void TrainerPanel::UpdateClosedState()
{
	if (mOpen)
		return;

	// The corner button only shows on the lawn and the main menu, where nothing else sits in that corner
	bool aShow = mApp->mDialogList.empty() && !mApp->mChallengeScreen && !mApp->mAwardScreen && !mApp->mCreditScreen &&
		(mApp->mBoard != nullptr || mApp->mGameSelector != nullptr);
	int aWidth = gTrainer.mBrush != TRAINER_BRUSH_NONE ? 360 : 92;
	if (aShow != mVisible || mWidth != aWidth)
	{
		mVisible = aShow;
		mMouseVisible = aShow;
		mDoFinger = true;
		Resize(4, mApp->mHeight - 20, aWidth, 17);
		MarkDirty();
	}
}

void TrainerPanel::Update()
{
	Widget::Update();
	if (!mOpen)
		UpdateClosedState();
	if (mVisible)
		MarkDirty();
}

int TrainerPanel::ItemAt(int x, int y)
{
	for (int i = 0; i < static_cast<int>(mItems.size()); i++)
	{
		if (mItems[i].mAction && mItems[i].mRect.Contains(x, y))
			return i;
	}
	return -1;
}

void TrainerPanel::MouseMove(int x, int y)
{
	Widget::MouseMove(x, y);
	mHoverItem = mOpen ? ItemAt(x, y) : -1;
}

void TrainerPanel::MouseDown(int x, int y, int theBtnNum, int /*theClickCount*/)
{
	if (theBtnNum != 0)
		return;

	if (!mOpen)
	{
		Open();
		return;
	}

	int anIndex = ItemAt(x, y);
	if (anIndex < 0)
		return;

	mApp->PlaySample(Sexy::SOUND_TAP);
	mItems[anIndex].mAction();
	MarkDirty();
}

void TrainerPanel::KeyDown(KeyCode theKey)
{
	if (mOpen && theKey == KeyCode::KEYCODE_ESCAPE)
		Close();
}

void TrainerPanel::Draw(Graphics* g)
{
	if (!mOpen)
	{
		bool anActive = gTrainer.AnyActive();
		g->SetColor(anActive ? Color(90, 60, 0, 200) : Color(0, 0, 0, 150));
		g->FillRect(0, 0, mWidth, mHeight);
		g->SetColor(Color(150, 200, 90));
		g->DrawRect(0, 0, mWidth - 1, mHeight - 1);
		g->SetFont(Sexy::FONT_DWARVENTODCRAFT12);
		g->SetColor(anActive ? Color(255, 230, 80) : Color(255, 255, 255));
		if (gTrainer.mBrush != TRAINER_BRUSH_NONE)
			g->DrawString(std::format("Placing {}. Right-click to stop", TrainerBrushName()), 6, 13);
		else
			g->DrawString(anActive ? "Trainer ON" : "Trainer", 6, 13);
		return;
	}

	g->SetColor(Color(0, 0, 0, 210));
	g->FillRect(0, 0, mWidth, mHeight);

	g->SetFont(Sexy::FONT_DWARVENTODCRAFT24);
	g->SetColor(Color(255, 230, 80));
	g->DrawString("PvZ Trainer", 40, 48);

	g->SetFont(Sexy::FONT_DWARVENTODCRAFT15);
	g->SetColor(Color(200, 200, 200));
	g->DrawString("Close with the Close button, Esc, or the key under Esc. The game waits while this is open.", 40, 72);

	g->SetColor(Color(140, 220, 90));
	g->DrawString("Switches", 40, 90);
	g->DrawString("Actions", 420, 90);
	g->DrawString("Level maker", 420, 352);
	g->DrawString("Put a plant on the lawn  (Mouse: click the lawn to place, right-click stops)", 40, 430);
	g->DrawString("Send a zombie", 40, 490);

	_Font* aFont = Sexy::FONT_DWARVENTODCRAFT15;
	g->SetFont(aFont);
	int aTextOffsetY = (ITEM_HEIGHT + aFont->GetAscent()) / 2 - 1;

	for (int i = 0; i < static_cast<int>(mItems.size()); i++)
	{
		const Item& anItem = mItems[i];
		const Rect& aRect = anItem.mRect;

		if (anItem.mAction)
		{
			g->SetColor(i == mHoverItem ? Color(80, 120, 45) : Color(40, 70, 25));
			g->FillRect(aRect);
			g->SetColor(Color(150, 200, 90));
			g->DrawRect(aRect.mX, aRect.mY, aRect.mWidth - 1, aRect.mHeight - 1);
		}

		if (anItem.mArrowLeft || anItem.mArrowRight)
		{
			g->SetColor(Color(255, 255, 255));
			DrawArrow(g, aRect, anItem.mArrowLeft);
			continue;
		}

		if (!anItem.mLabel)
			continue;

		std::string aLabel = anItem.mLabel();
		int aTextX;
		if (anItem.mChecked)
		{
			Rect aBox(aRect.mX + 5, aRect.mY + 4, ITEM_HEIGHT - 8, ITEM_HEIGHT - 8);
			g->SetColor(Color(255, 255, 255));
			g->DrawRect(aBox.mX, aBox.mY, aBox.mWidth - 1, aBox.mHeight - 1);
			if (*anItem.mChecked)
			{
				g->SetColor(Color(255, 230, 80));
				g->FillRect(aBox.mX + 3, aBox.mY + 3, aBox.mWidth - 6, aBox.mHeight - 6);
			}
			aTextX = aRect.mX + ITEM_HEIGHT + 4;
		}
		else
		{
			aTextX = aRect.mX + (aRect.mWidth - aFont->StringWidth(aLabel)) / 2;
		}

		g->SetColor(anItem.mChecked && *anItem.mChecked ? Color(255, 230, 80) : Color(255, 255, 255));
		g->DrawString(aLabel, aTextX, aRect.mY + aTextOffsetY);
	}

	if (!mStatus.empty())
	{
		g->SetColor(Color(255, 230, 80));
		g->DrawString(mStatus, 40, 568);
	}
}

Board* TrainerPanel::PlayingBoard()
{
	Board* aBoard = mApp->mBoard;
	if (aBoard == nullptr || mApp->mGameScene != GameScenes::SCENE_PLAYING ||
		mApp->mGameMode == GameMode::GAMEMODE_CHALLENGE_ZEN_GARDEN || mApp->mGameMode == GameMode::GAMEMODE_TREE_OF_WISDOM)
	{
		mStatus = "This only works while a level is being played.";
		return nullptr;
	}
	return aBoard;
}

void TrainerPanel::DoAddSun()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;
	aBoard->mSunMoney = std::min(aBoard->mSunMoney + 1000, 9990);
	mStatus = std::format("Sun is now {}.", aBoard->mSunMoney);
}

void TrainerPanel::DoKillAllZombies()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;

	int aCount = 0;
	for (Zombie* aZombie : aBoard->mZombies)
	{
		if (aZombie->mDead || aZombie->IsDeadOrDying() || !aZombie->IsOnBoard())
			continue;
		aZombie->TakeDamage(1000000, 1U << DamageFlags::DAMAGE_BYPASSES_SHIELD);
		aCount++;
	}
	mStatus = std::format("Hit {} zombies. Close the panel to watch them fall.", aCount);
}

void TrainerPanel::DoNextWave()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;

	if (gTrainer.mStopZombieWaves)
	{
		mStatus = "Turn off Stop Zombie Waves first.";
		return;
	}
	if (aBoard->mHugeWaveCountDown > 0)
		aBoard->mHugeWaveCountDown = 1;
	else
		aBoard->mZombieCountDown = 6;
	mStatus = "The next wave comes as soon as you close the panel.";
}

void TrainerPanel::DoWinLevel()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;

	if (aBoard->HasLevelAwardDropped() || aBoard->mBoardFadeOutCounter > 0)
	{
		mStatus = "This level is already finishing.";
		return;
	}
	aBoard->CheatCompleteLevel();
	mStatus = "Level won. Close the panel to continue.";
}

void TrainerPanel::DoAddCoins(int theAmount)
{
	if (mApp->mPlayerInfo == nullptr)
	{
		mStatus = "Pick a player profile first.";
		return;
	}
	mApp->mPlayerInfo->AddCoins(theAmount);
	mApp->WriteCurrentUserConfig();
	mStatus = std::format("Money is now {}.", mApp->mPlayerInfo->mCoins * 10);
}

void TrainerPanel::DoMaxCoins()
{
	if (mApp->mPlayerInfo == nullptr)
	{
		mStatus = "Pick a player profile first.";
		return;
	}
	mApp->mPlayerInfo->mCoins = MAX_COINS;
	mApp->WriteCurrentUserConfig();
	mStatus = std::format("Money is now {}.", mApp->mPlayerInfo->mCoins * 10);
}

void TrainerPanel::DoSpawnPlant(bool theFillLawn)
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;

	std::string aName = Plant::GetNameString(static_cast<SeedType>(mPlantType));
	if (theFillLawn)
	{
		int aCount = 0;
		for (int aRow = 0; aRow < MAX_GRID_SIZE_Y; aRow++)
		{
			for (int aCol = 0; aCol < MAX_GRID_SIZE_X; aCol++)
			{
				if (TrainerPlacePlant(aBoard, mPlantType, aCol, aRow, true))
					aCount++;
			}
		}
		mStatus = std::format("Planted {} {}.", aCount, aName);
	}
	else if (TrainerPlacePlant(aBoard, mPlantType, mPlantCol, mPlantRow, false))
	{
		mStatus = std::format("Planted {} at column {}, row {}.", aName, mPlantCol + 1, mPlantRow + 1);
	}
	else
	{
		mStatus = "That plant cannot go there. Try another spot or turn on Plant Anywhere.";
	}
}

void TrainerPanel::DoSpawnZombie()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;

	int aType = mZombieTypes[mZombieIndex];
	int aRow = mZombieRow >= 0 ? mZombieRow : aBoard->PickRowForNewZombie(static_cast<ZombieType>(aType));
	float aPosX = mZombieCol >= 0 ? static_cast<float>(aBoard->GridToPixelX(mZombieCol, aRow) + 10) : -1.0f;
	if (!TrainerPlaceZombie(aBoard, aType, aRow, aPosX))
	{
		mStatus = "That zombie cannot go in that row (water zombies need a water row).";
		return;
	}
	mStatus = mZombieCol >= 0 ? std::format("Sent a zombie to column {}, row {}.", mZombieCol + 1, aRow + 1) :
		std::format("Sent a zombie to row {}.", aRow + 1);
}

void TrainerPanel::DoStartBrush(int theBrush)
{
	if (PlayingBoard() == nullptr)
		return;

	gTrainer.mBrushType = theBrush == TRAINER_BRUSH_PLANT ? mPlantType : mZombieTypes[mZombieIndex];
	Close();
	gTrainer.mBrush = theBrush; // set after Close(), which would otherwise clear it
	UpdateClosedState();
}

void TrainerPanel::DoClearLawn()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;
	mStatus = std::format("Removed {} plants and zombies.", TrainerClearLawn(aBoard));
}

void TrainerPanel::DoStartBlankLevel()
{
	if (!TrainerCanStartLevel(mApp))
	{
		mStatus = "Go to the main menu or a level first.";
		return;
	}
	int aStage = mStage;
	Close();
	TrainerStartBlankLevel(mApp, aStage);
}

void TrainerPanel::DoSaveLevel()
{
	Board* aBoard = PlayingBoard();
	if (aBoard == nullptr)
		return;
	TrainerSaveLayout(aBoard, mSlot, mStatus);
}

void TrainerPanel::DoLoadLevel()
{
	if (!TrainerCanStartLevel(mApp))
	{
		mStatus = "Go to the main menu or a level first.";
		return;
	}
	Close();
	std::string aMessage;
	if (!TrainerLoadLayout(mApp, mSlot, aMessage))
	{
		Open();
		mStatus = aMessage;
	}
}
