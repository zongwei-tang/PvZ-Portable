/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local trainer patch).
 */

#ifndef __TRAINER_H__
#define __TRAINER_H__

#include <string>

class Board;
class LawnApp;

enum TrainerBrush
{
	TRAINER_BRUSH_NONE,
	TRAINER_BRUSH_PLANT,
	TRAINER_BRUSH_ZOMBIE,
};

// Runtime switches of the built-in trainer panel (see Widget/TrainerPanel).
// They are not saved: every launch starts with all of them off.
struct TrainerSettings
{
	bool	mInfiniteSun = false;
	bool	mNoCooldown = false;
	bool	mFreePlanting = false;
	bool	mPlantAnywhere = false;
	bool	mInvinciblePlants = false;
	bool	mFreezeZombies = false;
	bool	mStopZombieWaves = false;
	bool	mAutoCollect = false;
	bool	mAllPlants = false;
	bool	mTenSeedSlots = false;
	int		mGameSpeed = 1;

	// Level maker: a blank level is a survival stage with waves held back that never touches
	// the player's real survival save or records.
	bool	mSandbox = false;
	int		mBrush = TRAINER_BRUSH_NONE;	// what a left click on the lawn places
	int		mBrushType = 0;					// SeedType or ZombieType of the brush

	bool AnyActive() const
	{
		return mInfiniteSun || mNoCooldown || mFreePlanting || mPlantAnywhere || mInvinciblePlants || mFreezeZombies ||
			mStopZombieWaves || mAutoCollect || mAllPlants || mTenSeedSlots || mGameSpeed != 1;
	}
};

extern TrainerSettings gTrainer;

constexpr int TRAINER_NUM_STAGES = 5;	// day, night, pool, fog, roof
constexpr int TRAINER_NUM_SLOTS = 5;

const char*		TrainerStageName(int theStage);
bool			TrainerPlacePlant(Board* theBoard, int theSeedType, int theCol, int theRow, bool theOnlyEmpty);
bool			TrainerPlaceZombie(Board* theBoard, int theZombieType, int theRow, float thePosX);	// thePosX < 0: right edge
bool			TrainerBrushClick(Board* theBoard, int x, int y);
std::string		TrainerBrushName();
int				TrainerClearLawn(Board* theBoard);
bool			TrainerCanStartLevel(LawnApp* theApp);
void			TrainerStartBlankLevel(LawnApp* theApp, int theStage);
bool			TrainerSaveLayout(Board* theBoard, int theSlot, std::string& theMessage);
bool			TrainerLoadLayout(LawnApp* theApp, int theSlot, std::string& theMessage);
void			TrainerApplyPendingLayout(Board* theBoard);
void			TrainerLeaveSandbox();

#endif
