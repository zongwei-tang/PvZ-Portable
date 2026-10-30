/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifdef PVZP_CLOUD_SAVE

#include "CloudSaveDialog.h"
#include "GameButton.h"
#include "../System/CloudSave.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../PvzpLib/PvzpCommon.h"
#include "../../PvzpLib/PvzpStringFile.h"
#include "graphics/Font.h"
#include "widget/Checkbox.h"
#include "widget/WidgetManager.h"
#include <algorithm>

namespace
{
constexpr int SYNC_BUTTON_ID = 0;
constexpr int ENABLED_CHECKBOX_ID = 0;
constexpr int LABEL_WIDTH = 130;		// labels sit right-aligned in this column, left of the fields
constexpr int FIELD_WIDTH = 330;
constexpr int FIELD_HEIGHT = 28;
constexpr int FIELD_SPACING = 42;
constexpr int STATUS_HEIGHT = 48;		// two lines of status text
constexpr int MAX_FIELD_CHARS = 256;

const char* const FIELD_LABELS[CloudSaveDialog::NUM_FIELDS] = { "Server URL", "Username", "Password" };

int SyncButtonWidth()
{
	return IMAGE_BUTTON_LEFT->mWidth + IMAGE_BUTTON_MIDDLE->mWidth * 4 + IMAGE_BUTTON_RIGHT->mWidth;
}
}

CloudSaveEditWidget::CloudSaveEditWidget(int theId, CloudSaveDialog* theOwner) : LawnEditWidget(theId, theOwner, theOwner), mOwner(theOwner)
{
	StyleEditWidget(this);
	mAutoCapFirstLetter = false;
	mMaxChars = MAX_FIELD_CHARS;
}

void CloudSaveEditWidget::KeyDown(KeyCode theKey)
{
	if (theKey == KeyCode::KEYCODE_TAB)
	{
		mOwner->FocusNextField(mId);
		return;
	}
	LawnEditWidget::KeyDown(theKey);
}

void CloudSaveEditWidget::Draw(Graphics* g)
{
	if (!mMasked)
	{
		LawnEditWidget::Draw(g);
		return;
	}

	// one star per byte keeps the cursor and selection offsets valid
	std::string aText = std::move(mString);
	mString.assign(aText.size(), '*');
	LawnEditWidget::Draw(g);
	mString = std::move(aText);
}

CloudSaveDialog::CloudSaveDialog(LawnApp* theApp) : LawnDialog(
	theApp,
	Dialogs::DIALOG_CLOUD_SAVE_SETTINGS,
	true,
	"Cloud Save",
	"Keep your saves on a WebDAV server, such as Nextcloud or Jianguoyun, to continue on another computer.",
	"",
	Dialog::BUTTONS_YES_NO)
{
	CloudSync::Config aConfig = CloudSave::GetSettings();
	const std::string* aValues[NUM_FIELDS] = { &aConfig.mUrl, &aConfig.mUsername, &aConfig.mPassword };
	for (int i = 0; i < NUM_FIELDS; i++)
	{
		mFields[i] = std::make_unique<CloudSaveEditWidget>(i, this);
		mFields[i]->SetText(*aValues[i], true);
	}
	mFields[FIELD_PASSWORD]->mMasked = true;

	mEnabledCheckbox = MakeNewCheckbox(ENABLED_CHECKBOX_ID, this, aConfig.mEnabled);
	mSyncButton = MakeButton(SYNC_BUTTON_ID, this, "Sync Now");
	if (mLawnYesButton)
		mLawnYesButton->mLabel = "Save";
	if (mLawnNoButton)
		mLawnNoButton->mLabel = PvzpStringTranslate("[DIALOG_BUTTON_CANCEL]");

	mStatus = CloudSave::StatusText();
	mVerticalCenterText = false;

	int aContentWidth = LABEL_WIDTH + FIELD_WIDTH + 16;
	int aContentHeight = IMAGE_OPTIONS_CHECKBOX0->mHeight + NUM_FIELDS * FIELD_SPACING + STATUS_HEIGHT + mSyncButton->mHeight + 20;
	CalcSize(std::max(aContentWidth - mHeaderFont->StringWidth(mDialogHeader), 0), aContentHeight);
}

CloudSaveDialog::~CloudSaveDialog() = default;

void CloudSaveDialog::Show(LawnApp* theApp)
{
	CloudSaveDialog* aDialog = new CloudSaveDialog(theApp);
	LawnApp::CenterDialog(aDialog, aDialog->mWidth, aDialog->mHeight);
	theApp->AddDialog(Dialogs::DIALOG_CLOUD_SAVE_SETTINGS, aDialog);
	theApp->mWidgetManager->SetFocus(aDialog->mFields[FIELD_URL].get());
}

void CloudSaveDialog::Resize(int theX, int theY, int theWidth, int theHeight)
{
	LawnDialog::Resize(theX, theY, theWidth, theHeight);
	if (!mSyncButton || !mLawnYesButton)
		return;

	// laid out upwards from the Save/Cancel buttons
	int aLeft = mBackgroundInsets.mLeft + mContentInsets.mLeft + 8;
	int aFieldX = aLeft + LABEL_WIDTH;
	int aFieldWidth = mWidth - mBackgroundInsets.mRight - mContentInsets.mRight - 16 - aFieldX;

	int aSyncWidth = SyncButtonWidth();
	int aSyncY = mLawnYesButton->mY - 10 - mSyncButton->mHeight;
	mSyncButton->Resize((mWidth - aSyncWidth) / 2, aSyncY, aSyncWidth, mSyncButton->mHeight);

	mStatusY = aSyncY - STATUS_HEIGHT;
	int aFirstFieldY = mStatusY - NUM_FIELDS * FIELD_SPACING + (FIELD_SPACING - FIELD_HEIGHT) / 2;
	for (int i = 0; i < NUM_FIELDS; i++)
		mFields[i]->Resize(aFieldX, aFirstFieldY + i * FIELD_SPACING, aFieldWidth, FIELD_HEIGHT);

	int aBoxSize = IMAGE_OPTIONS_CHECKBOX0->mHeight;
	mEnabledCheckbox->Resize(aFieldX - 8, aFirstFieldY - aBoxSize - 4, IMAGE_OPTIONS_CHECKBOX0->mWidth, aBoxSize);
}

void CloudSaveDialog::AddedToManager(WidgetManager* theWidgetManager)
{
	LawnDialog::AddedToManager(theWidgetManager);
	for (auto& aField : mFields)
		AddWidget(aField.get());
	AddWidget(mEnabledCheckbox.get());
	AddWidget(mSyncButton.get());
}

void CloudSaveDialog::RemovedFromManager(WidgetManager* theWidgetManager)
{
	LawnDialog::RemovedFromManager(theWidgetManager);
	for (auto& aField : mFields)
		RemoveWidget(aField.get());
	RemoveWidget(mEnabledCheckbox.get());
	RemoveWidget(mSyncButton.get());
}

void CloudSaveDialog::Draw(Graphics* g)
{
	LawnDialog::Draw(g);

	int aLabelX = mFields[FIELD_URL]->mX - 16;
	for (int i = 0; i < NUM_FIELDS; i++)
	{
		DrawEditBox(g, mFields[i].get());
		PvzpDrawString(g, FIELD_LABELS[i], aLabelX, mFields[i]->mY + 21, FONT_DWARVENTODCRAFT18, mColors.mLines, DrawStringJustification::DS_ALIGN_RIGHT);
	}
	PvzpDrawString(g, "Enabled", aLabelX, mEnabledCheckbox->mY + mEnabledCheckbox->mHeight / 2 + 8, FONT_DWARVENTODCRAFT18, mColors.mLines, DrawStringJustification::DS_ALIGN_RIGHT);

	int aLeft = mBackgroundInsets.mLeft + mContentInsets.mLeft;
	int aWidth = mWidth - aLeft - mBackgroundInsets.mRight - mContentInsets.mRight;
	PvzpDrawStringWrapped(g, mStatus, Rect(aLeft, mStatusY + 8, aWidth, STATUS_HEIGHT - 8), FONT_DWARVENTODCRAFT15, mColors.mLines, DrawStringJustification::DS_ALIGN_CENTER);
}

void CloudSaveDialog::KeyDown(KeyCode theKey)
{
	// not LawnDialog::KeyDown: Space, Y and N are text here, not buttons
	if (theKey == KeyCode::KEYCODE_ESCAPE)
		ButtonDepress(Dialog::ID_NO);
	else if (theKey == KeyCode::KEYCODE_RETURN)
		ButtonDepress(Dialog::ID_YES);
	else if (theKey == KeyCode::KEYCODE_TAB)
		FocusNextField(NUM_FIELDS - 1);
}

void CloudSaveDialog::FocusNextField(int theId)
{
	mWidgetManager->SetFocus(mFields[(theId + 1) % NUM_FIELDS].get());
}

void CloudSaveDialog::EditWidgetText(int theId, [[maybe_unused]] const std::string& theString)
{
	// Enter: on to the next field, and save from the last one
	if (theId == FIELD_PASSWORD)
		ButtonDepress(Dialog::ID_YES);
	else
		FocusNextField(theId);
}

void CloudSaveDialog::CheckboxChecked([[maybe_unused]] int theId, [[maybe_unused]] bool theChecked)
{
	mApp->PlaySample(SOUND_BUTTONCLICK);
}

CloudSync::Config CloudSaveDialog::ReadFields() const
{
	CloudSync::Config aConfig = CloudSave::GetSettings();
	aConfig.mEnabled = mEnabledCheckbox->mChecked;
	aConfig.mUrl = mFields[FIELD_URL]->mString;
	aConfig.mUsername = mFields[FIELD_USERNAME]->mString;
	aConfig.mPassword = mFields[FIELD_PASSWORD]->mString;
	if (aConfig.mFolder.empty())
		aConfig.mFolder = CloudSync::Config().mFolder;
	return aConfig;
}

bool CloudSaveDialog::Apply()
{
	CloudSync::Config aConfig = ReadFields();
	if (aConfig.mEnabled && !aConfig.mUrl.starts_with("https://") && !aConfig.mUrl.starts_with("http://"))
	{
		mStatus = "The server URL must start with https:// or http://.";
		mWidgetManager->SetFocus(mFields[FIELD_URL].get());
		return false;
	}
	if (!CloudSave::ApplySettings(aConfig))
	{
		mStatus = "Could not write cloudsave.ini.";
		return false;
	}
	mStatus = CloudSave::StatusText();
	return true;
}

void CloudSaveDialog::ButtonDepress(int theId)
{
	switch (theId)
	{
	case SYNC_BUTTON_ID:
		if (!mEnabledCheckbox->mChecked)
		{
			mStatus = "Tick Enabled to sync.";
			break;
		}
		if (Apply())
		{
			CloudSave::SyncNow();
			mStatus = CloudSave::StatusText();
		}
		break;

	case Dialog::ID_YES:
		if (Apply())
			mApp->KillDialog(mId);
		break;

	case Dialog::ID_NO:
		mApp->KillDialog(mId);
		break;
	}
}

#endif
