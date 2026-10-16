/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local trainer patch).
 */

#include "Trainer.h"
#include "Music.h"
#include "PlayerInfo.h"
#include "../Board.h"
#include "../Plant.h"
#include "../Zombie.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../PvzpLib/PvzpFoley.h"
#include "../../PvzpLib/PvzpStringFile.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

TrainerSettings gTrainer;

namespace
{
struct LayoutPlant
{
	int		mSeedType;
	int		mImitaterType;
	int		mCol;
	int		mRow;
};

struct LayoutZombie
{
	int		mZombieType;
	int		mRow;
	float	mPosX;
};

struct Layout
{
	int							mStage = 0;
	int							mSun = 50;
	std::vector<LayoutPlant>	mPlants;
	std::vector<LayoutZombie>	mZombies;
};

bool gHasPendingLayout = false;
Layout gPendingLayout;

constexpr GameMode STAGE_MODES[TRAINER_NUM_STAGES] = {
	GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_1,
	GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_2,
	GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_3,
	GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_4,
	GameMode::GAMEMODE_SURVIVAL_NORMAL_STAGE_5,
};
constexpr const char* STAGE_NAMES[TRAINER_NUM_STAGES] = { "Day", "Night", "Pool", "Fog", "Roof" };
constexpr const char* LAYOUT_HEADER = "PVZ-PORTABLE-TRAINER-LEVEL 1";

struct SettingsSwitch
{
	const char*	mKey;
	bool		TrainerSettings::*mValue;
};

// mStopZombieWaves is written separately because a blank level forces it on
constexpr SettingsSwitch SETTINGS_SWITCHES[] = {
	{ "infinite_sun", &TrainerSettings::mInfiniteSun },
	{ "no_cooldown", &TrainerSettings::mNoCooldown },
	{ "free_planting", &TrainerSettings::mFreePlanting },
	{ "plant_anywhere", &TrainerSettings::mPlantAnywhere },
	{ "invincible_plants", &TrainerSettings::mInvinciblePlants },
	{ "freeze_zombies", &TrainerSettings::mFreezeZombies },
	{ "auto_collect", &TrainerSettings::mAutoCollect },
	{ "all_plants", &TrainerSettings::mAllPlants },
	{ "ten_seed_slots", &TrainerSettings::mTenSeedSlots },
};

std::string SettingsPath()
{
	return Sexy::GetAppDataPath("trainer.ini");
}

std::string SlotPath(int theSlot)
{
	return Sexy::GetAppDataPath(std::format("trainer-levels/level{}.txt", theSlot + 1));
}

int StageForBoard(Board* theBoard)
{
	switch (theBoard->mBackground)
	{
	case BackgroundType::BACKGROUND_2_NIGHT:	return 1;
	case BackgroundType::BACKGROUND_3_POOL:		return 2;
	case BackgroundType::BACKGROUND_4_FOG:		return 3;
	case BackgroundType::BACKGROUND_5_ROOF:
	case BackgroundType::BACKGROUND_6_BOSS:		return 4;
	default:									return 0;
	}
}

int NumRows(Board* theBoard)
{
	return theBoard->StageHas6Rows() ? 6 : 5;
}

bool IsAquaticZombie(int theType)
{
	return theType == ZombieType::ZOMBIE_SNORKEL || theType == ZombieType::ZOMBIE_DOLPHIN_RIDER;
}

bool IsValidZombieType(int theType)
{
	return theType >= 0 && theType < ZombieType::NUM_ZOMBIE_TYPES && theType != ZombieType::ZOMBIE_BOSS;
}
}

const char* TrainerStageName(int theStage)
{
	return STAGE_NAMES[std::clamp(theStage, 0, TRAINER_NUM_STAGES - 1)];
}

bool TrainerPlacePlant(Board* theBoard, int theSeedType, int theCol, int theRow, bool theOnlyEmpty)
{
	if (theCol < 0 || theCol >= MAX_GRID_SIZE_X || theRow < 0 || theRow >= NumRows(theBoard))
		return false;

	SeedType aSeedType = static_cast<SeedType>(theSeedType);
	if (aSeedType == SeedType::SEED_COBCANNON && theCol >= MAX_GRID_SIZE_X - 1)
		return false;

	// Fill Lawn keeps to the normal rules even with Plant Anywhere on, so it only fills empty spots
	bool aPlantAnywhere = gTrainer.mPlantAnywhere;
	if (theOnlyEmpty)
		gTrainer.mPlantAnywhere = false;
	bool aCanPlant = theBoard->CanPlantAt(theCol, theRow, aSeedType) == PlantingReason::PLANTING_OK;
	gTrainer.mPlantAnywhere = aPlantAnywhere;
	if (!aCanPlant)
		return false;

	Plant* aPlant = theBoard->AddPlant(theCol, theRow, aSeedType, SeedType::SEED_NONE);
	if (aPlant)
		theBoard->DoPlantingEffects(theCol, theRow, aPlant);
	return aPlant != nullptr;
}

bool TrainerPlaceZombie(Board* theBoard, int theZombieType, int theRow, float thePosX)
{
	if (!IsValidZombieType(theZombieType) || theRow < 0 || theRow >= MAX_GRID_SIZE_Y || !theBoard->RowCanHaveZombies(theRow))
		return false;
	if (IsAquaticZombie(theZombieType) && theBoard->mPlantRow[theRow] != PlantRowType::PLANTROW_POOL)
		return false;

	Zombie* aZombie = theBoard->AddZombieInRow(static_cast<ZombieType>(theZombieType), theRow, Zombie::ZOMBIE_WAVE_DEBUG);
	if (aZombie == nullptr)
		return false;

	if (thePosX >= 0.0f)
	{
		aZombie->mPosX = thePosX;
		aZombie->mPosY = aZombie->GetPosYBasedOnRow(theRow);
	}
	return true;
}

bool TrainerBrushClick(Board* theBoard, int x, int y)
{
	if (gTrainer.mBrush == TRAINER_BRUSH_NONE)
		return false;

	int aCol = theBoard->PixelToGridX(x, y);
	int aRow = theBoard->PixelToGridY(x, y);
	if (aCol < 0 || aRow < 0)
		return false;

	bool aPlaced = gTrainer.mBrush == TRAINER_BRUSH_PLANT ?
		TrainerPlacePlant(theBoard, gTrainer.mBrushType, aCol, aRow, false) :
		TrainerPlaceZombie(theBoard, gTrainer.mBrushType, aRow, static_cast<float>(x - 40));
	if (!aPlaced)
		theBoard->mApp->PlaySample(Sexy::SOUND_BUZZER);
	return true;
}

std::string TrainerBrushName()
{
	if (gTrainer.mBrush == TRAINER_BRUSH_PLANT)
		return Plant::GetNameString(static_cast<SeedType>(gTrainer.mBrushType));
	if (gTrainer.mBrush == TRAINER_BRUSH_ZOMBIE)
		return TrainerZombieName(gTrainer.mBrushType);
	return std::string();
}

std::string TrainerZombieName(int theZombieType)
{
	// The Zombotany zombies share the plain "ZOMBIE" name, which made them look like one entry
	switch (theZombieType)
	{
	case ZombieType::ZOMBIE_PEA_HEAD:		return "Peashooter Zombie";
	case ZombieType::ZOMBIE_WALLNUT_HEAD:	return "Wall-nut Zombie";
	case ZombieType::ZOMBIE_JALAPENO_HEAD:	return "Jalapeno Zombie";
	case ZombieType::ZOMBIE_GATLING_HEAD:	return "Gatling Pea Zombie";
	case ZombieType::ZOMBIE_SQUASH_HEAD:	return "Squash Zombie";
	case ZombieType::ZOMBIE_TALLNUT_HEAD:	return "Tall-nut Zombie";
	default:								break;
	}

	const ZombieDefinition& aDef = GetZombieDefinition(static_cast<ZombieType>(theZombieType));
	std::string aKey = std::format("[{}]", aDef.mZombieName);
	if (PvzpStringListExists(aKey))
		return std::string(PvzpStringTranslate(aKey));
	if (theZombieType == ZombieType::ZOMBIE_REDEYE_GARGANTUAR)
		return "Giga-gargantuar";
	return aDef.mZombieName;
}

void TrainerLoadSettings()
{
	std::ifstream aFile(Sexy::PathFromU8(SettingsPath()));
	std::string aLine;
	while (std::getline(aFile, aLine))
	{
		size_t anEquals = aLine.find('=');
		if (anEquals == std::string::npos)
			continue;
		std::string aKey = aLine.substr(0, anEquals);
		int aValue = 0;
		std::istringstream aStream(aLine.substr(anEquals + 1));
		if (!(aStream >> aValue))
			continue;

		for (const SettingsSwitch& aSwitch : SETTINGS_SWITCHES)
		{
			if (aKey == aSwitch.mKey)
				gTrainer.*aSwitch.mValue = aValue != 0;
		}
		if (aKey == "stop_zombie_waves")
		{
			gTrainer.mStopZombieWavesOutsideSandbox = aValue != 0;
			if (!gTrainer.mSandbox)
				gTrainer.mStopZombieWaves = aValue != 0;
		}
		else if (aKey == "game_speed" && std::ranges::find(TRAINER_GAME_SPEEDS, aValue) != std::end(TRAINER_GAME_SPEEDS))
		{
			gTrainer.mGameSpeed = aValue;
		}
	}
}

void TrainerSaveSettings()
{
	std::ostringstream aText;
	for (const SettingsSwitch& aSwitch : SETTINGS_SWITCHES)
		aText << aSwitch.mKey << "=" << (gTrainer.*aSwitch.mValue ? 1 : 0) << "\n";
	bool aStopZombieWaves = gTrainer.mSandbox ? gTrainer.mStopZombieWavesOutsideSandbox : gTrainer.mStopZombieWaves;
	aText << "stop_zombie_waves=" << (aStopZombieWaves ? 1 : 0) << "\n";
	aText << "game_speed=" << gTrainer.mGameSpeed << "\n";

	std::string aData = aText.str();
	gSexyAppBase->WriteBytesToFile(SettingsPath(), aData.data(), aData.size());
}

int TrainerClearLawn(Board* theBoard)
{
	int aCount = 0;
	for (Plant* aPlant : theBoard->mPlants)
	{
		if (aPlant->mDead)
			continue;
		aPlant->Die();
		aCount++;
	}
	for (Zombie* aZombie : theBoard->mZombies)
	{
		if (aZombie->mDead || aZombie->IsDeadOrDying() || !aZombie->IsOnBoard())
			continue;
		aZombie->DieNoLoot();
		aCount++;
	}
	return aCount;
}

bool TrainerCanStartLevel(LawnApp* theApp)
{
	if (theApp->mPlayerInfo == nullptr)
		return false;
	if (theApp->mBoard)
		return true;
	return theApp->mGameSelector && theApp->mDialogList.empty() && !theApp->mChallengeScreen && !theApp->mAwardScreen && !theApp->mCreditScreen;
}

void TrainerStartBlankLevel(LawnApp* theApp, int theStage)
{
	if (theApp->mBoard)
	{
		theApp->mMusic->StopAllMusic();
		theApp->mSoundSystem->CancelPausedFoley();
		theApp->mBoardResult = BoardResult::BOARDRESULT_NONE;
		theApp->KillBoard();
	}
	theApp->KillGameSelector();

	if (!gTrainer.mSandbox)
		gTrainer.mStopZombieWavesOutsideSandbox = gTrainer.mStopZombieWaves;
	gTrainer.mSandbox = true;
	gTrainer.mStopZombieWaves = true;
	gTrainer.mBrush = TRAINER_BRUSH_NONE;
	theApp->mGameMode = STAGE_MODES[std::clamp(theStage, 0, TRAINER_NUM_STAGES - 1)];
	theApp->NewGame();
}

bool TrainerSaveLayout(Board* theBoard, int theSlot, std::string& theMessage)
{
	Sexy::MkDir(Sexy::GetAppDataPath("trainer-levels"));
	std::ofstream aFile(Sexy::PathFromU8(SlotPath(theSlot)));
	if (!aFile)
	{
		theMessage = "Could not write the level file.";
		return false;
	}

	int aNumPlants = 0;
	int aNumZombies = 0;
	aFile << LAYOUT_HEADER << "\n";
	aFile << "stage " << StageForBoard(theBoard) + 1 << "\n";
	aFile << "sun " << theBoard->mSunMoney << "\n";
	for (Plant* aPlant : theBoard->mPlants)
	{
		if (aPlant->mDead)
			continue;
		aFile << "plant " << static_cast<int>(aPlant->mSeedType) << " " << static_cast<int>(aPlant->mImitaterType) << " " << aPlant->mPlantCol << " " << aPlant->mRow << "\n";
		aNumPlants++;
	}
	for (Zombie* aZombie : theBoard->mZombies)
	{
		if (aZombie->mDead || aZombie->IsDeadOrDying() || !aZombie->IsOnBoard() || !IsValidZombieType(aZombie->mZombieType))
			continue;
		aFile << "zombie " << static_cast<int>(aZombie->mZombieType) << " " << aZombie->mRow << " " << static_cast<int>(aZombie->mPosX) << "\n";
		aNumZombies++;
	}

	if (!aFile)
	{
		theMessage = "Could not write the level file.";
		return false;
	}
	theMessage = std::format("Saved {} plants and {} zombies to slot {}.", aNumPlants, aNumZombies, theSlot + 1);
	return true;
}

bool TrainerLoadLayout(LawnApp* theApp, int theSlot, std::string& theMessage)
{
	std::ifstream aFile(Sexy::PathFromU8(SlotPath(theSlot)));
	std::string aLine;
	if (!aFile || !std::getline(aFile, aLine) || aLine != LAYOUT_HEADER)
	{
		theMessage = std::format("Slot {} is empty.", theSlot + 1);
		return false;
	}

	Layout aLayout;
	while (std::getline(aFile, aLine))
	{
		std::istringstream aStream(aLine);
		std::string aKind;
		aStream >> aKind;
		if (aKind == "stage")
		{
			int aStage = 1;
			if (aStream >> aStage)
				aLayout.mStage = std::clamp(aStage - 1, 0, TRAINER_NUM_STAGES - 1);
		}
		else if (aKind == "sun")
		{
			int aSun = 50;
			if (aStream >> aSun)
				aLayout.mSun = std::clamp(aSun, 0, 9990);
		}
		else if (aKind == "plant")
		{
			LayoutPlant aPlant{};
			if (aStream >> aPlant.mSeedType >> aPlant.mImitaterType >> aPlant.mCol >> aPlant.mRow &&
				aPlant.mSeedType >= 0 && aPlant.mSeedType <= SeedType::SEED_IMITATER &&
				aPlant.mImitaterType >= SeedType::SEED_NONE && aPlant.mImitaterType < SeedType::SEED_IMITATER &&
				aPlant.mCol >= 0 && aPlant.mCol < MAX_GRID_SIZE_X && aPlant.mRow >= 0 && aPlant.mRow < MAX_GRID_SIZE_Y)
				aLayout.mPlants.push_back(aPlant);
		}
		else if (aKind == "zombie")
		{
			LayoutZombie aZombie{};
			int aPosX = 0;
			if (aStream >> aZombie.mZombieType >> aZombie.mRow >> aPosX && IsValidZombieType(aZombie.mZombieType) &&
				aZombie.mRow >= 0 && aZombie.mRow < MAX_GRID_SIZE_Y && aPosX > -200 && aPosX < 1200)
			{
				aZombie.mPosX = static_cast<float>(aPosX);
				aLayout.mZombies.push_back(aZombie);
			}
		}
	}

	if (!TrainerCanStartLevel(theApp))
	{
		theMessage = "Go to the main menu or a level first.";
		return false;
	}

	gPendingLayout = std::move(aLayout);
	gHasPendingLayout = true;
	TrainerStartBlankLevel(theApp, gPendingLayout.mStage);
	theMessage = std::format("Loaded slot {}.", theSlot + 1);
	return true;
}

void TrainerApplyPendingLayout(Board* theBoard)
{
	if (!gHasPendingLayout)
		return;
	gHasPendingLayout = false;
	if (!gTrainer.mSandbox)
		return;

	theBoard->mSunMoney = gPendingLayout.mSun;
	for (const LayoutPlant& aPlant : gPendingLayout.mPlants)
	{
		if (aPlant.mRow < NumRows(theBoard))
			theBoard->AddPlant(aPlant.mCol, aPlant.mRow, static_cast<SeedType>(aPlant.mSeedType), static_cast<SeedType>(aPlant.mImitaterType));
	}
	for (const LayoutZombie& aZombie : gPendingLayout.mZombies)
	{
		TrainerPlaceZombie(theBoard, aZombie.mZombieType, aZombie.mRow, aZombie.mPosX);
	}
	gPendingLayout = Layout();
}

void TrainerLeaveSandbox()
{
	if (gTrainer.mSandbox)
	{
		gTrainer.mSandbox = false;
		gTrainer.mStopZombieWaves = gTrainer.mStopZombieWavesOutsideSandbox;
	}
	gTrainer.mBrush = TRAINER_BRUSH_NONE;
	gHasPendingLayout = false;
}
