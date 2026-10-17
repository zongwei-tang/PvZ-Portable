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
// The switches and the game speed are kept in trainer.ini, so they carry over to the next launch.
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
	bool	mStopZombieWavesOutsideSandbox = false;	// the player's own Stop Zombie Waves while a blank level forces it on
	int		mBrush = TRAINER_BRUSH_NONE;	// what a left click on the lawn places
	int		mBrushType = 0;					// SeedType or ZombieType of the brush

	// The endless run being played used Jump, so it no longer raises the best-flags record.
	// Reset with every new board and restored from the save slot's info file on load.
	bool	mEndlessJumped = false;

	bool AnyActive() const
	{
		return mInfiniteSun || mNoCooldown || mFreePlanting || mPlantAnywhere || mInvinciblePlants || mFreezeZombies ||
			mStopZombieWaves || mAutoCollect || mAllPlants || mTenSeedSlots || mGameSpeed != 1;
	}
};

extern TrainerSettings gTrainer;

constexpr int TRAINER_GAME_SPEEDS[] = { 1, 2, 3, 5, 10 };
constexpr int TRAINER_NUM_STAGES = 5;	// day, night, pool, fog, roof
constexpr int TRAINER_NUM_SLOTS = 5;
constexpr int TRAINER_MAX_ENDLESS_FLAGS = 9998;

const char*		TrainerStageName(int theStage);
bool			TrainerPlacePlant(Board* theBoard, int theSeedType, int theCol, int theRow, bool theOnlyEmpty);
bool			TrainerPlaceZombie(Board* theBoard, int theZombieType, int theRow, float thePosX);	// thePosX < 0: right edge
bool			TrainerBrushClick(Board* theBoard, int x, int y);
std::string		TrainerBrushName();
std::string		TrainerZombieName(int theZombieType);
bool			TrainerSavedRunJumped(const std::string& theInfoPath);
void			TrainerLoadSettings();
void			TrainerSaveSettings();
int				TrainerClearLawn(Board* theBoard);
bool			TrainerJumpEndlessWave(Board* theBoard, int theFlags, int theWave, std::string& theMessage);	// theWave: waves into the next flag
bool			TrainerCanStartLevel(LawnApp* theApp);
void			TrainerStartBlankLevel(LawnApp* theApp, int theStage);
bool			TrainerSaveLayout(Board* theBoard, int theSlot, std::string& theMessage);
bool			TrainerLoadLayout(LawnApp* theApp, int theSlot, std::string& theMessage);
void			TrainerApplyPendingLayout(Board* theBoard);
void			TrainerLeaveSandbox();

#endif
