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
#include "../System/CloudSave.h"
#include "../System/CloudSync.h"
#include "widget/CheckboxListener.h"
#include "widget/EditListener.h"
#include <memory>

class CloudSaveDialog;

// One text field of the dialog: no auto capitals, Tab moves on, and a password is drawn as stars.
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

// Cloud save settings (cloudsave.ini): the service, its fields, a sign-in for Google Drive and OneDrive, and Sync Now.
// Opened from the options on the main menu only, because syncing may replace the loaded profiles.
class CloudSaveDialog : public LawnDialog, public EditListener, public Sexy::CheckboxListener
{
public:
	enum
	{
		NUM_FIELDS = 3,		// what they hold depends on the service: URL, username and password for WebDAV, the client for the others
	};

	std::unique_ptr<CloudSaveEditWidget>	mFields[NUM_FIELDS];
	std::unique_ptr<Sexy::Checkbox>			mEnabledCheckbox;
	std::unique_ptr<LawnStoneButton>		mServiceButton;
	std::unique_ptr<LawnStoneButton>		mSignInButton;
	std::unique_ptr<LawnStoneButton>		mSyncButton;
	CloudSync::Config						mDraft;			// the fields of every service, as typed so far
	CloudSync::Provider						mProvider;
	CloudSave::SignInState					mSignIn;
	int										mSeenCodeSerial;
	bool									mSignedIn = false;	// to mProvider, read from disk on changes only
	std::string								mStatus;
	bool									mStatusIsMessage = false;	// else mStatus follows the sync status
	std::string								mSeenSyncStatus;
	int										mStatusY = 0;		// the status text sits between these two
	int										mStatusBottom = 0;

public:
	explicit CloudSaveDialog(LawnApp* theApp);
	~CloudSaveDialog() override;

	static void			Show(LawnApp* theApp);

	void				Resize(int theX, int theY, int theWidth, int theHeight) override;
	void				AddedToManager(WidgetManager* theWidgetManager) override;
	void				RemovedFromManager(WidgetManager* theWidgetManager) override;
	void				Update() override;
	void				Draw(Graphics* g) override;
	void				KeyDown(KeyCode theKey) override;
	void				ButtonDepress(int theId) override;
	void				EditWidgetText(int theId, const std::string& theString) override;
	using LawnDialog::CheckboxChecked;
	void				CheckboxChecked(int theId, bool theChecked) override;
	void				FocusNextField(int theId);

private:
	int					FieldCount() const;			// the first this many fields are shown
	std::string*		FieldValue(int theField);	// where field theField of the shown service is kept in mDraft
	const char*			FieldLabel(int theField) const;
	void				StoreFields();
	void				ShowFields();
	void				UpdateButtons();
	void				ShowMessage(std::string theMessage);
	void				ShowIdleStatus();
	std::string			IdleStatus() const;
	bool				SigningIn() const;
	CloudSync::Config	ReadFields();
	bool				Apply(bool theForSignIn);
	void				Close(bool theStopSignIn);
};

#endif

#endif
