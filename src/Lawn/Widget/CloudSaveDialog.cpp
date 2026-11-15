/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifdef PVZP_CLOUD_SAVE

#include "CloudSaveDialog.h"
#include "GameButton.h"
#include "../../LawnApp.h"
#include "../../Resources.h"
#include "../../PvzpLib/PvzpCommon.h"
#include "../../PvzpLib/PvzpStringFile.h"
#include "graphics/Font.h"
#include "widget/Checkbox.h"
#include "widget/WidgetManager.h"
#include <algorithm>
#include <format>
#include <initializer_list>
#include <SDL.h>

namespace
{
using Phase = CloudSave::SignInState::Phase;

constexpr int SYNC_BUTTON_ID = 0;
constexpr int SIGN_IN_BUTTON_ID = 1;
constexpr int SERVICE_BUTTON_ID = 2;
constexpr int ENABLED_CHECKBOX_ID = 0;
constexpr int LABEL_WIDTH = 130;		// labels sit right-aligned in this column, left of the fields
constexpr int FIELD_WIDTH = 330;
constexpr int FIELD_HEIGHT = 28;
constexpr int FIELD_SPACING = 42;
constexpr int SERVICE_ROW_HEIGHT = 44;
constexpr int STATUS_HEIGHT = 66;		// three lines of status text below the last field, more when fields are hidden
constexpr int BUTTON_GAP = 12;
constexpr int MAX_FIELD_CHARS = 256;

constexpr const char* SIGN_IN_LABEL = "Sign In";
constexpr const char* SIGN_OUT_LABEL = "Sign Out";
constexpr const char* STOP_LABEL = "Stop Sign-In";
constexpr const char* SYNC_LABEL = "Sync Now";

// Wide enough for the widest of theLabels, in whole middle tiles as DrawStoneButton() draws them.
int StoneButtonWidth(std::initializer_list<std::string_view> theLabels)
{
	int aTextWidth = 0;
	for (std::string_view aLabel : theLabels)
		aTextWidth = std::max(aTextWidth, FONT_DWARVENTODCRAFT18GREENINSET->StringWidth(aLabel));
	int anEnds = IMAGE_BUTTON_LEFT->mWidth + IMAGE_BUTTON_RIGHT->mWidth;
	int aMiddle = IMAGE_BUTTON_MIDDLE->mWidth;
	int aTiles = std::max(1, (aTextWidth + 24 - anEnds + aMiddle - 1) / aMiddle);
	return anEnds + aTiles * aMiddle;
}

std::string Trim(const std::string& theText)
{
	size_t aStart = theText.find_first_not_of(" \t\r\n");
	if (aStart == std::string::npos)
		return "";
	size_t anEnd = theText.find_last_not_of(" \t\r\n");
	return theText.substr(aStart, anEnd - aStart + 1);
}

// Whether the sign-in of theConfig.mProvider has what it needs, typed in or built into this build.
bool HasClient(const CloudSync::Config& theConfig)
{
	switch (theConfig.mProvider)
	{
	case CloudSync::Provider::GOOGLE_DRIVE:
		return !CloudSync::GoogleClientId(theConfig).empty() && !CloudSync::GoogleClientSecret(theConfig).empty();
	case CloudSync::Provider::ONEDRIVE:
		return !CloudSync::OneDriveClientId(theConfig).empty();
	default:
		return true;
	}
}

const char* ClientHint(CloudSync::Provider theProvider)
{
	if (theProvider == CloudSync::Provider::GOOGLE_DRIVE)
		return "Enter the client ID and client secret of your Google Cloud app (see the README), then Sign In.";
	return "Enter the application (client) ID of your Microsoft app registration (see the README), then Sign In.";
}

// "https://www.google.com/device" -> "google.com/device", shorter to read and to type on another device
std::string SiteText(std::string_view theUrl)
{
	for (std::string_view aPrefix : { "https://", "http://" })
		if (theUrl.starts_with(aPrefix))
			theUrl.remove_prefix(aPrefix.size());
	if (theUrl.starts_with("www."))
		theUrl.remove_prefix(4);
	if (theUrl.ends_with("/"))
		theUrl.remove_suffix(1);
	return std::string(theUrl);
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
	"Keep your saves in the cloud to continue on another computer.",
	"",
	Dialog::BUTTONS_YES_NO),
	mDraft(CloudSave::GetSettings()),
	mOpened(mDraft),
	mProvider(mDraft.mProvider),
	mSignIn(CloudSave::GetSignIn()),
	mSeenCodeSerial(mSignIn.mCodeSerial)	// a code handed out before the dialog opened does not open the browser again
{
	for (int i = 0; i < NUM_FIELDS; i++)
		mFields[i] = std::make_unique<CloudSaveEditWidget>(i, this);
	mEnabledCheckbox = MakeNewCheckbox(ENABLED_CHECKBOX_ID, this, mDraft.mEnabled);
	mServiceButton = MakeButton(SERVICE_BUTTON_ID, this, CloudSync::ProviderName(mProvider));
	mSignInButton = MakeButton(SIGN_IN_BUTTON_ID, this, SIGN_IN_LABEL);
	mSyncButton = MakeButton(SYNC_BUTTON_ID, this, SYNC_LABEL);
	if (mLawnYesButton)
		mLawnYesButton->mLabel = "Save";
	if (mLawnNoButton)
		mLawnNoButton->mLabel = PvzpStringTranslate("[DIALOG_BUTTON_CANCEL]");

	ShowFields();
	mSignedIn = CloudSave::IsSignedIn(mProvider);
	UpdateButtons();
	ShowIdleStatus();
	mVerticalCenterText = false;

	int aContentWidth = LABEL_WIDTH + FIELD_WIDTH + 16;
	int aContentHeight = SERVICE_ROW_HEIGHT + NUM_FIELDS * FIELD_SPACING + STATUS_HEIGHT + mSyncButton->mHeight + 20;
	CalcSize(std::max(aContentWidth - mHeaderFont->StringWidth(mDialogHeader), 0), aContentHeight);
}

// A sign-in still running goes on without the dialog after Save, so a code typed in just before still counts.
CloudSaveDialog::~CloudSaveDialog() = default;

void CloudSaveDialog::Show(LawnApp* theApp)
{
	CloudSaveDialog* aDialog = new CloudSaveDialog(theApp);
	LawnApp::CenterDialog(aDialog, aDialog->mWidth, aDialog->mHeight);
	theApp->AddDialog(Dialogs::DIALOG_CLOUD_SAVE_SETTINGS, aDialog);
	theApp->mWidgetManager->SetFocus(aDialog->mFields[0].get());
}

int CloudSaveDialog::FieldCount() const
{
	switch (mProvider)
	{
	case CloudSync::Provider::WEBDAV:		return 3;	// server URL, username, password
	case CloudSync::Provider::GOOGLE_DRIVE:	return 2;	// client ID, client secret
	case CloudSync::Provider::ONEDRIVE:		return 1;	// client ID
	}
	return 0;
}

std::string* CloudSaveDialog::FieldValue(int theField)
{
	if (theField < 0 || theField >= FieldCount())
		return nullptr;
	switch (mProvider)
	{
	case CloudSync::Provider::WEBDAV:
	{
		std::string* aValues[] = { &mDraft.mUrl, &mDraft.mUsername, &mDraft.mPassword };
		return aValues[theField];
	}
	case CloudSync::Provider::GOOGLE_DRIVE:
		return theField == 0 ? &mDraft.mGoogleClientId : &mDraft.mGoogleClientSecret;
	case CloudSync::Provider::ONEDRIVE:
		return &mDraft.mOneDriveClientId;
	}
	return nullptr;
}

const char* CloudSaveDialog::FieldLabel(int theField) const
{
	switch (mProvider)
	{
	case CloudSync::Provider::WEBDAV:
	{
		const char* const aLabels[] = { "Server URL", "Username", "Password" };
		return aLabels[std::clamp(theField, 0, 2)];
	}
	case CloudSync::Provider::GOOGLE_DRIVE:
		return theField == 0 ? "Client ID" : "Client Secret";
	case CloudSync::Provider::ONEDRIVE:
		return "Client ID";
	}
	return "";
}

void CloudSaveDialog::StoreFields()
{
	for (int i = 0; i < FieldCount(); i++)
	{
		bool aPassword = mProvider == CloudSync::Provider::WEBDAV && i == 2;
		*FieldValue(i) = aPassword ? mFields[i]->mString : Trim(mFields[i]->mString);
	}
}

void CloudSaveDialog::ShowFields()
{
	for (int i = 0; i < NUM_FIELDS; i++)
	{
		std::string* aValue = FieldValue(i);
		mFields[i]->SetText(aValue ? *aValue : std::string(), true);
		mFields[i]->mMasked = mProvider == CloudSync::Provider::WEBDAV && i == 2;
		mFields[i]->SetVisible(aValue != nullptr);
	}
	if (mWidgetManager && std::any_of(std::begin(mFields), std::end(mFields), [this](const auto& aField)
		{ return !aField->mVisible && mWidgetManager->mFocusWidget == aField.get(); }))
		mWidgetManager->SetFocus(mFields[0].get());
}

void CloudSaveDialog::UpdateButtons()
{
	mSignInButton->SetLabel(SigningIn() ? STOP_LABEL : mSignedIn ? SIGN_OUT_LABEL : SIGN_IN_LABEL);
}

void CloudSaveDialog::ShowMessage(std::string theMessage)
{
	mStatus = std::move(theMessage);
	mStatusIsMessage = true;
}

void CloudSaveDialog::ShowIdleStatus()
{
	mStatusIsMessage = false;
	mSeenSyncStatus = CloudSave::StatusText();
	mStatus = IdleStatus();
}

std::string CloudSaveDialog::IdleStatus() const
{
	// the sync status describes the service last saved, which need not be the one shown
	bool aSaved = CloudSave::GetSettings().mProvider == mProvider;
	if (!CloudSync::IsOAuth(mProvider))
		return aSaved ? CloudSave::StatusText() : "Enter your WebDAV server and account, then Save.";

	if (!mSignedIn)
	{
		CloudSync::Config aConfig = mDraft;
		aConfig.mProvider = mProvider;
		if (!HasClient(aConfig))
			return ClientHint(mProvider);
		return std::format("Click Sign In to connect your {} account.",
			mProvider == CloudSync::Provider::GOOGLE_DRIVE ? "Google" : "Microsoft");
	}
	std::string anAccount = CloudSave::AccountText(mProvider);
	return aSaved ? anAccount + "\n" + CloudSave::StatusText() : anAccount;
}

bool CloudSaveDialog::SigningIn() const
{
	return mSignIn.mPhase == Phase::STARTING || mSignIn.mPhase == Phase::WAITING;
}

void CloudSaveDialog::Resize(int theX, int theY, int theWidth, int theHeight)
{
	LawnDialog::Resize(theX, theY, theWidth, theHeight);
	if (!mSyncButton || !mLawnYesButton)
		return;

	int aLeft = mBackgroundInsets.mLeft + mContentInsets.mLeft + 8;
	int aRight = mWidth - mBackgroundInsets.mRight - mContentInsets.mRight - 16;
	int aFieldX = aLeft + LABEL_WIDTH;

	// the row above Save and Cancel
	bool anOAuth = CloudSync::IsOAuth(mProvider);
	int aRowY = mLawnYesButton->mY - 10 - mSyncButton->mHeight;
	int aSyncWidth = StoneButtonWidth({ SYNC_LABEL });
	int aSignInWidth = StoneButtonWidth({ SIGN_IN_LABEL, SIGN_OUT_LABEL, STOP_LABEL });
	int aRowX = (mWidth - aSyncWidth - (anOAuth ? aSignInWidth + BUTTON_GAP : 0)) / 2;
	if (anOAuth)
	{
		mSignInButton->Resize(aRowX, aRowY, aSignInWidth, mSignInButton->mHeight);
		aRowX += aSignInWidth + BUTTON_GAP;
	}
	mSyncButton->Resize(aRowX, aRowY, aSyncWidth, mSyncButton->mHeight);
	mSignInButton->SetVisible(anOAuth);
	mStatusBottom = aRowY;

	// top down from here: the service and Enabled, the fields of the service, then the status in what is left
	int aTop = aRowY - STATUS_HEIGHT - NUM_FIELDS * FIELD_SPACING - SERVICE_ROW_HEIGHT;
	int aServiceWidth = StoneButtonWidth({ CloudSync::ProviderName(CloudSync::Provider::WEBDAV),
		CloudSync::ProviderName(CloudSync::Provider::GOOGLE_DRIVE), CloudSync::ProviderName(CloudSync::Provider::ONEDRIVE) });
	mServiceButton->Resize(aFieldX, aTop + (SERVICE_ROW_HEIGHT - mServiceButton->mHeight) / 2, aServiceWidth, mServiceButton->mHeight);

	int aBoxWidth = IMAGE_OPTIONS_CHECKBOX0->mWidth;
	int aBoxHeight = IMAGE_OPTIONS_CHECKBOX0->mHeight;
	mEnabledCheckbox->Resize(aRight - aBoxWidth, aTop + (SERVICE_ROW_HEIGHT - aBoxHeight) / 2, aBoxWidth, aBoxHeight);

	int aFieldY = aTop + SERVICE_ROW_HEIGHT + (FIELD_SPACING - FIELD_HEIGHT) / 2;
	for (int i = 0; i < NUM_FIELDS; i++)
		mFields[i]->Resize(aFieldX, aFieldY + i * FIELD_SPACING, aRight - aFieldX, FIELD_HEIGHT);
	mStatusY = aTop + SERVICE_ROW_HEIGHT + FieldCount() * FIELD_SPACING;
}

void CloudSaveDialog::AddedToManager(WidgetManager* theWidgetManager)
{
	LawnDialog::AddedToManager(theWidgetManager);
	for (auto& aField : mFields)
		AddWidget(aField.get());
	AddWidget(mEnabledCheckbox.get());
	AddWidget(mServiceButton.get());
	AddWidget(mSignInButton.get());
	AddWidget(mSyncButton.get());
}

void CloudSaveDialog::RemovedFromManager(WidgetManager* theWidgetManager)
{
	LawnDialog::RemovedFromManager(theWidgetManager);
	for (auto& aField : mFields)
		RemoveWidget(aField.get());
	RemoveWidget(mEnabledCheckbox.get());
	RemoveWidget(mServiceButton.get());
	RemoveWidget(mSignInButton.get());
	RemoveWidget(mSyncButton.get());
}

void CloudSaveDialog::Update()
{
	LawnDialog::Update();

	CloudSave::SignInState aSignIn = CloudSave::GetSignIn();
	if (aSignIn.mCodeSerial != mSeenCodeSerial)
	{
		mSeenCodeSerial = aSignIn.mCodeSerial;
		if (aSignIn.mPhase == Phase::WAITING)
		{
			// the code can be pasted into the page that opens, or typed in on another device
			SDL_SetClipboardText(aSignIn.mUserCode.c_str());
			if (aSignIn.mUrl.starts_with("https://"))
				SDL_OpenURL(aSignIn.mUrl.c_str());
		}
	}

	bool aChanged = aSignIn.mPhase != mSignIn.mPhase;
	mSignIn = std::move(aSignIn);
	if (aChanged)
	{
		if (mSignIn.mPhase == Phase::DONE)
		{
			mSignedIn = CloudSave::IsSignedIn(mProvider);
			ShowIdleStatus();
		}
		else if (mSignIn.mPhase == Phase::FAILED)
			ShowMessage(mSignIn.mMessage);
		UpdateButtons();
	}

	if (!mStatusIsMessage && CloudSave::StatusText() != mSeenSyncStatus)
		ShowIdleStatus();
}

void CloudSaveDialog::Draw(Graphics* g)
{
	LawnDialog::Draw(g);

	int aLabelX = mFields[0]->mX - 16;
	PvzpDrawString(g, "Service", aLabelX, mServiceButton->mY + 23, FONT_DWARVENTODCRAFT18, mColors.mLines, DrawStringJustification::DS_ALIGN_RIGHT);
	PvzpDrawString(g, "Enabled", mEnabledCheckbox->mX - 8, mEnabledCheckbox->mY + mEnabledCheckbox->mHeight / 2 + 8, FONT_DWARVENTODCRAFT18, mColors.mLines, DrawStringJustification::DS_ALIGN_RIGHT);
	for (int i = 0; i < FieldCount(); i++)
	{
		DrawEditBox(g, mFields[i].get());
		PvzpDrawString(g, FieldLabel(i), aLabelX, mFields[i]->mY + 21, FONT_DWARVENTODCRAFT18, mColors.mLines, DrawStringJustification::DS_ALIGN_RIGHT);
	}

	int aLeft = mBackgroundInsets.mLeft + mContentInsets.mLeft;
	int aWidth = mWidth - aLeft - mBackgroundInsets.mRight - mContentInsets.mRight;
	int aHeight = mStatusBottom - mStatusY;
	if (mSignIn.mPhase == Phase::WAITING)
	{
		// what to do, the code in big letters, and where else it is
		constexpr int CODE_BLOCK_HEIGHT = 80;
		int aY = mStatusY + std::max(0, (aHeight - CODE_BLOCK_HEIGHT) / 2);
		int aCenterX = aLeft + aWidth / 2;
		PvzpDrawString(g, std::format("Go to {} and enter:", SiteText(mSignIn.mUrl)), aCenterX, aY + 18, FONT_DWARVENTODCRAFT15, mColors.mLines, DrawStringJustification::DS_ALIGN_CENTER);
		PvzpDrawString(g, mSignIn.mUserCode, aCenterX, aY + 52, FONT_DWARVENTODCRAFT24, mColors.mLines, DrawStringJustification::DS_ALIGN_CENTER);
		PvzpDrawString(g, "The code is on the clipboard too.", aCenterX, aY + 74, FONT_DWARVENTODCRAFT15, mColors.mLines, DrawStringJustification::DS_ALIGN_CENTER);
		return;
	}

	const std::string& aText = SigningIn() ? mSignIn.mMessage : mStatus;
	PvzpDrawStringWrapped(g, aText, Rect(aLeft, mStatusY + 8, aWidth, std::max(aHeight - 8, 0)), FONT_DWARVENTODCRAFT15, mColors.mLines, DrawStringJustification::DS_ALIGN_CENTER);
}

void CloudSaveDialog::KeyDown(KeyCode theKey)
{
	// not LawnDialog::KeyDown: Space, Y and N are text here, not buttons
	if (theKey == KeyCode::KEYCODE_ESCAPE)
		ButtonDepress(Dialog::ID_NO);
	else if (theKey == KeyCode::KEYCODE_RETURN)
		ButtonDepress(Dialog::ID_YES);
	else if (theKey == KeyCode::KEYCODE_TAB)
		FocusNextField(FieldCount() - 1);
}

void CloudSaveDialog::FocusNextField(int theId)
{
	mWidgetManager->SetFocus(mFields[(theId + 1) % std::max(FieldCount(), 1)].get());
}

void CloudSaveDialog::EditWidgetText(int theId, [[maybe_unused]] const std::string& theString)
{
	// Enter: on to the next field, and save from the last one
	if (theId >= FieldCount() - 1)
		ButtonDepress(Dialog::ID_YES);
	else
		FocusNextField(theId);
}

void CloudSaveDialog::CheckboxChecked([[maybe_unused]] int theId, [[maybe_unused]] bool theChecked)
{
	mApp->PlaySample(SOUND_BUTTONCLICK);
}

CloudSync::Config CloudSaveDialog::ReadFields()
{
	StoreFields();
	CloudSync::Config aConfig = mDraft;
	aConfig.mEnabled = mEnabledCheckbox->mChecked;
	aConfig.mProvider = mProvider;
	if (aConfig.mFolder.empty())
		aConfig.mFolder = CloudSync::Config().mFolder;
	return aConfig;
}

// Saves the fields to cloudsave.ini and restarts syncing with them. Signing in needs the client of the service even
// while Enabled is not ticked, since the sign-in is made with the settings last saved.
bool CloudSaveDialog::Apply(bool theForSignIn)
{
	CloudSync::Config aConfig = ReadFields();
	if (aConfig.mProvider == CloudSync::Provider::WEBDAV)
	{
		if (aConfig.mEnabled && !aConfig.mUrl.starts_with("https://") && !aConfig.mUrl.starts_with("http://"))
		{
			ShowMessage("The server URL must start with https:// or http://.");
			mWidgetManager->SetFocus(mFields[0].get());
			return false;
		}
	}
	else if ((aConfig.mEnabled || theForSignIn) && !HasClient(aConfig))
	{
		ShowMessage(ClientHint(aConfig.mProvider));
		bool aSecretMissing = aConfig.mProvider == CloudSync::Provider::GOOGLE_DRIVE && !CloudSync::GoogleClientId(aConfig).empty();
		mWidgetManager->SetFocus(mFields[aSecretMissing ? 1 : 0].get());
		return false;
	}

	if (!CloudSave::ApplySettings(aConfig))
	{
		ShowMessage("Could not write cloudsave.ini.");
		return false;
	}
	mSavedForSignIn = theForSignIn;
	return true;
}

// Cancel also takes back what Sign In saved, so trying another service never quietly stops the one in use.
// The sign-in itself is kept: picking that service later and saving needs no new one.
void CloudSaveDialog::Close(bool theCancel)
{
	if (theCancel && SigningIn())
		CloudSave::CancelSignIn();
	if (theCancel && mSavedForSignIn)
		CloudSave::ApplySettings(mOpened);
	mApp->KillDialog(mId);
}

void CloudSaveDialog::ButtonDepress(int theId)
{
	switch (theId)
	{
	case SERVICE_BUTTON_ID:
		if (SigningIn())
		{
			ShowMessage("Stop the sign-in first to pick another service.");
			break;
		}
		StoreFields();
		mProvider = static_cast<CloudSync::Provider>((static_cast<int>(mProvider) + 1) % CloudSync::NUM_PROVIDERS);
		mServiceButton->SetLabel(CloudSync::ProviderName(mProvider));
		ShowFields();
		mSignedIn = CloudSave::IsSignedIn(mProvider);
		UpdateButtons();
		ShowIdleStatus();
		Resize(mX, mY, mWidth, mHeight);
		mApp->PlaySample(SOUND_BUTTONCLICK);
		break;

	case SIGN_IN_BUTTON_ID:
		if (SigningIn())
		{
			CloudSave::CancelSignIn();
			mSignIn = CloudSave::GetSignIn();
			ShowMessage("Sign-in stopped.");
		}
		else if (mSignedIn)
		{
			CloudSave::SignOut(mProvider);
			mSignedIn = CloudSave::IsSignedIn(mProvider);
			ShowMessage(std::format("Signed out of {} on this computer.", CloudSync::ProviderName(mProvider)));
		}
		else if (Apply(true))
		{
			if (CloudSave::StartSignIn(mProvider))
				mSignIn = CloudSave::GetSignIn();
			else
				ShowMessage("The last sign-in is still stopping. Try again in a moment.");
		}
		UpdateButtons();
		break;

	case SYNC_BUTTON_ID:
		if (!mEnabledCheckbox->mChecked)
			ShowMessage("Tick Enabled to sync.");
		else if (CloudSync::IsOAuth(mProvider) && !mSignedIn)
			ShowMessage(std::format("Sign in to {} first.", CloudSync::ProviderName(mProvider)));
		else if (Apply(false))
		{
			CloudSave::SyncNow();
			ShowIdleStatus();
		}
		break;

	case Dialog::ID_YES:
		if (Apply(false))
			Close(false);
		break;

	case Dialog::ID_NO:
		Close(true);
		break;
	}
}

#endif
