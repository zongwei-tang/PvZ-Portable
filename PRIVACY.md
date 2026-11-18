# Privacy policy of PvZ-Portable cloud save

This covers the cloud save of PvZ-Portable when you sign in with Google Drive or OneDrive.

## What the game accesses

- **Google Drive:** only the hidden app data folder of PvZ-Portable (the `drive.appdata` scope). The game cannot see, read or change any other file in your Drive.
- **OneDrive:** only the app folder of PvZ-Portable (the `Files.ReadWrite.AppFolder` scope), plus a refresh token so that you stay signed in (`offline_access`).

The game keeps your save files there and reads them back on your other computers. It does not read your name, email address, contacts or any other data of your account.

## Where your data goes

- Your saves go only to your own Google Drive or OneDrive, and come back only to the game on your computers.
- The sign-in token is stored only on your computer, in the per-user data folder of the game. It is not sent anywhere but to Google or Microsoft, to keep the sign-in working.
- There is no server of PvZ-Portable. The developers never receive your saves, your token or any data about you, and nothing is shared with third parties, sold or used for ads.

## Removing access

- **Sign Out** in the Cloud Save dialog deletes the token on that computer.
- To revoke the access itself, remove PvZ-Portable at <https://myaccount.google.com/permissions> or <https://account.live.com/consent/Manage>.
- The saves in the app folder stay in your Drive or OneDrive until you delete them. Removing the app's access in Google Drive's settings, under **Manage apps**, offers to delete its hidden data.

## Contact

Open an issue at <https://github.com/zongwei-tang/PvZ-Portable/issues>.
