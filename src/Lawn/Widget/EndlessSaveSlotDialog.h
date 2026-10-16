/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local endless save slots patch).
 */

#ifndef __ENDLESSSAVESLOTDIALOG_H__
#define __ENDLESSSAVESLOTDIALOG_H__

#include "LawnDialog.h"
#include "../LawnCommon.h"
#include <memory>

class LawnStoneButton;

// Shown when an endless survival is picked on the challenge screen: each endless stage keeps
// NUM_ENDLESS_SAVE_SLOTS separate runs, and this chooses which one to continue or start.
class EndlessSaveSlotDialog : public LawnDialog
{
public:
	GameMode							mGameMode;
	std::unique_ptr<LawnStoneButton>	mSlotButtons[NUM_ENDLESS_SAVE_SLOTS];

public:
	EndlessSaveSlotDialog(LawnApp* theApp, GameMode theGameMode);
	~EndlessSaveSlotDialog() override;

	void				Resize(int theX, int theY, int theWidth, int theHeight) override;
	void				AddedToManager(WidgetManager* theWidgetManager) override;
	void				RemovedFromManager(WidgetManager* theWidgetManager) override;
	void				KeyDown(KeyCode theKey) override;
	void				ButtonDepress(int theId) override;

private:
	std::string			SlotLabel(int theSlot);
};

#endif
