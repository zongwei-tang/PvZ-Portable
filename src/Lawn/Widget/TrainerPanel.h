/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local trainer patch).
 */

#ifndef __TRAINERPANEL_H__
#define __TRAINERPANEL_H__

#include "widget/Widget.h"
#include <functional>
#include <string>
#include <vector>

class LawnApp;
class Board;

// Built-in trainer: a full-screen overlay toggled with the key under Esc, F1, or a small "Trainer"
// button in the bottom-left corner of the lawn and the main menu (it turns yellow while a switch is on).
class TrainerPanel : public Sexy::Widget
{
public:
	struct Item
	{
		Sexy::Rect					mRect;
		std::function<std::string()>	mLabel;
		std::function<void()>		mAction;	// null for plain labels
		const bool*					mChecked;	// non-null for switches
		bool						mArrowLeft;
		bool						mArrowRight;
	};

	LawnApp*						mApp;
	std::vector<Item>				mItems;
	bool							mOpen;
	bool							mPausedBoard;
	Sexy::Widget*					mPrevFocus;
	int								mHoverItem;
	std::string						mStatus;
	int								mPlantType;
	int								mPlantCol;
	int								mPlantRow;
	int								mZombieIndex;
	int								mZombieRow;	// -1 = random row
	int								mZombieCol;	// -1 = right edge
	int								mStage;
	int								mSlot;
	int								mEndlessFlags;	// flags already done when jumping in endless survival
	int								mEndlessWave;	// waves into the next flag
	std::vector<int>				mZombieTypes;

public:
	explicit TrainerPanel(LawnApp* theApp);

	void							Toggle();
	void							Open();
	void							Close();
	void							UpdateClosedState();

	void							Update() override;
	void							Draw(Sexy::Graphics* g) override;
	void							MouseMove(int x, int y) override;
	void							MouseDown(int x, int y, int theBtnNum, int theClickCount) override;
	void							KeyDown(Sexy::KeyCode theKey) override;

private:
	void							BuildItems();
	void							AddSwitch(int theX, int theY, std::string theLabel, bool* theValue);
	void							AddButton(int theX, int theY, int theWidth, std::string theLabel, std::function<void()> theAction);
	void							AddSelector(int theX, int theY, int theWidth, std::function<std::string()> theLabel, std::function<void(int)> theStep);
	int								ItemAt(int x, int y);
	Board*							PlayingBoard();

	void							DoAddSun();
	void							DoKillAllZombies();
	void							DoNextWave();
	void							DoWinLevel();
	void							DoAddCoins(int theAmount);
	void							DoMaxCoins();
	void							DoSpawnPlant(bool theFillLawn);
	void							DoSpawnZombie();
	void							DoStartBrush(int theBrush);
	void							DoClearLawn();
	void							DoStartBlankLevel();
	void							DoSaveLevel();
	void							DoLoadLevel();
	void							DoJumpEndlessWave();
};

#endif
