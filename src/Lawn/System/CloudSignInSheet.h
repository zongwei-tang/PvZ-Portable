/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#ifndef __CLOUDSIGNINSHEET_H__
#define __CLOUDSIGNINSHEET_H__

#include <string>

// The sign-in page of Google or Microsoft in the system's sign-in sheet (ASWebAuthenticationSession) on iOS. Opening it
// in Safari would send the game to the background, where iOS soon stops it, and with it the loopback listener that
// the page comes back to. The sheet keeps the game in front: the page comes back to the listener as on a desktop, and
// the game closes the sheet once it has the answer. Other systems open the page in the browser instead.
namespace CloudSignInSheet
{

#if defined(PVZP_CLOUD_SAVE) && defined(__APPLE__) && defined(PVZP_CLOUD_SIGN_IN_SHEET)
bool	Open(const std::string& thePage);	// on the main thread; false if the sheet could not be shown
void	Close();							// the game closed it: not taken for the player's cancel
bool	TakeCancelled();					// once: the player closed the sheet before the sign-in was done
#else
inline bool	Open(const std::string&) { return false; }
inline void	Close() {}
inline bool	TakeCancelled() { return false; }
#endif

}

#endif
