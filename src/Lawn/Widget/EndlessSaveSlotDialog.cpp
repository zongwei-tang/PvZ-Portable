/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local endless save slots patch).
 */

#include "EndlessSaveSlotDialog.h"
#include "GameButton.h"
#include "../System/PlayerInfo.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "graphics/Font.h"
#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>

namespace
{
constexpr int SLOT_BUTTON_GAP = 6;

int SlotButtonWidth()
{
	return IMAGE_BUTTON_LEFT->mWidth + IMAGE_BUTTON_MIDDLE->mWidth * 6 + IMAGE_BUTTON_RIGHT->mWidth;
}
}

EndlessSaveSlotDialog::EndlessSaveSlotDialog(LawnApp* theApp, GameMode theGameMode) : LawnDialog(
	theApp,
	Dialogs::DIALOG_ENDLESS_SAVE_SLOT,
	true,
	"Save Slot",
	"Each endless stage keeps its own saves. Pick one to continue or start.",
	"[DIALOG_BUTTON_CANCEL]",
	Dialog::BUTTONS_FOOTER),
	mGameMode(theGameMode)
{
	for (int i = 0; i < NUM_ENDLESS_SAVE_SLOTS; i++)
		mSlotButtons[i] = MakeButton(i, this, SlotLabel(i));

	mTallBottom = true;
	// Wide enough for the slot buttons, tall enough for their stack above the Cancel button
	int aSlotsWidth = SlotButtonWidth() - mHeaderFont->StringWidth(mDialogHeader);
	int aSlotsHeight = NUM_ENDLESS_SAVE_SLOTS * (mSlotButtons[0]->mHeight + SLOT_BUTTON_GAP);
	CalcSize(std::max(aSlotsWidth, 0), aSlotsHeight + 10);
}

EndlessSaveSlotDialog::~EndlessSaveSlotDialog() = default;

std::string EndlessSaveSlotDialog::SlotLabel(int theSlot)
{
	int aProfileId = mApp->mPlayerInfo->mId;
	bool aHasSave = mApp->FileExists(GetSavedGameName(mGameMode, aProfileId, theSlot)) ||
		(theSlot == 0 && mApp->FileExists(GetLegacySavedGameName(mGameMode, aProfileId)));
	if (!aHasSave)
		return std::format("Slot {}: Empty", theSlot + 1);

	std::ifstream anInfo(Sexy::PathFromU8(GetSavedGameInfoName(mGameMode, aProfileId, theSlot)));
	std::string aLine;
	while (std::getline(anInfo, aLine))
	{
		int aFlags = 0;
		if (std::sscanf(aLine.c_str(), "flags=%d", &aFlags) == 1)
			return std::format("Slot {}: {} Flags", theSlot + 1, aFlags);
	}
	return std::format("Slot {}: Saved", theSlot + 1);
}

void EndlessSaveSlotDialog::Resize(int theX, int theY, int theWidth, int theHeight)
{
	LawnDialog::Resize(theX, theY, theWidth, theHeight);
	if (!mSlotButtons[0] || !mLawnYesButton)
		return;

	int aButtonWidth = SlotButtonWidth();
	int aButtonHeight = mSlotButtons[0]->mHeight;
	int aButtonX = (mWidth - aButtonWidth) / 2;
	int aButtonY = mLawnYesButton->mY - 10 - NUM_ENDLESS_SAVE_SLOTS * (aButtonHeight + SLOT_BUTTON_GAP);
	for (int i = 0; i < NUM_ENDLESS_SAVE_SLOTS; i++)
		mSlotButtons[i]->Resize(aButtonX, aButtonY + i * (aButtonHeight + SLOT_BUTTON_GAP), aButtonWidth, aButtonHeight);
}

void EndlessSaveSlotDialog::AddedToManager(WidgetManager* theWidgetManager)
{
	LawnDialog::AddedToManager(theWidgetManager);
	for (auto& aButton : mSlotButtons)
		AddWidget(aButton.get());
}

void EndlessSaveSlotDialog::RemovedFromManager(WidgetManager* theWidgetManager)
{
	LawnDialog::RemovedFromManager(theWidgetManager);
	for (auto& aButton : mSlotButtons)
		RemoveWidget(aButton.get());
}

void EndlessSaveSlotDialog::KeyDown(KeyCode theKey)
{
	if (theKey == KeyCode::KEYCODE_ESCAPE)
	{
		ButtonDepress(Dialog::ID_FOOTER);
		return;
	}

	int aSlot = static_cast<int>(theKey) - '1';
	if (aSlot >= 0 && aSlot < NUM_ENDLESS_SAVE_SLOTS)
		ButtonDepress(aSlot);
}

void EndlessSaveSlotDialog::ButtonDepress(int theId)
{
	LawnApp* anApp = mApp;
	if (theId >= 0 && theId < NUM_ENDLESS_SAVE_SLOTS)
	{
		GameMode aGameMode = mGameMode;
		anApp->mEndlessSaveSlot = theId;
		anApp->KillDialog(mId);
		anApp->KillChallengeScreen();
		anApp->PreNewGame(aGameMode, true);
		return;
	}

	anApp->KillDialog(mId);
}
