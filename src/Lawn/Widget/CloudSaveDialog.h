/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSAVEDIALOG_H__
#define __CLOUDSAVEDIALOG_H__

#ifdef PVZP_CLOUD_SAVE

#include "LawnDialog.h"
#include "../LawnCommon.h"
#include "../System/CloudSync.h"
#include "widget/CheckboxListener.h"
#include "widget/EditListener.h"
#include <memory>

class CloudSaveDialog;

// One text field of the dialog: no auto capitals, Tab moves on, and the password is drawn as stars.
class CloudSaveEditWidget : public LawnEditWidget
{
public:
	CloudSaveDialog*	mOwner;
	bool				mMasked = false;

public:
	CloudSaveEditWidget(int theId, CloudSaveDialog* theOwner);

	void				KeyDown(KeyCode theKey) override;
	void				Draw(Graphics* g) override;
};

// Cloud save settings (cloudsave.ini) with a Sync Now button. Opened from the options on the main menu only,
// because syncing may replace the loaded profiles.
class CloudSaveDialog : public LawnDialog, public EditListener, public Sexy::CheckboxListener
{
public:
	enum
	{
		FIELD_URL,
		FIELD_USERNAME,
		FIELD_PASSWORD,
		NUM_FIELDS,
	};

	std::unique_ptr<CloudSaveEditWidget>	mFields[NUM_FIELDS];
	std::unique_ptr<Sexy::Checkbox>			mEnabledCheckbox;
	std::unique_ptr<LawnStoneButton>		mSyncButton;
	std::string								mStatus;
	int										mStatusY = 0;

public:
	explicit CloudSaveDialog(LawnApp* theApp);
	~CloudSaveDialog() override;

	static void			Show(LawnApp* theApp);

	void				Resize(int theX, int theY, int theWidth, int theHeight) override;
	void				AddedToManager(WidgetManager* theWidgetManager) override;
	void				RemovedFromManager(WidgetManager* theWidgetManager) override;
	void				Draw(Graphics* g) override;
	void				KeyDown(KeyCode theKey) override;
	void				ButtonDepress(int theId) override;
	void				EditWidgetText(int theId, const std::string& theString) override;
	using LawnDialog::CheckboxChecked;
	void				CheckboxChecked(int theId, bool theChecked) override;
	void				FocusNextField(int theId);

private:
	CloudSync::Config	ReadFields() const;
	bool				Apply();
};

#endif

#endif
