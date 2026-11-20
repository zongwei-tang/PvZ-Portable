/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#if defined(PVZP_CLOUD_SAVE) && defined(PVZP_CLOUD_SIGN_IN_SHEET)

#import <AuthenticationServices/AuthenticationServices.h>
#import <UIKit/UIKit.h>

#include "CloudSignInSheet.h"

#if !__has_feature(objc_arc)
#error CloudSignInSheet.mm must be built with ARC (-fobjc-arc)
#endif

@interface PvzpSignInAnchor : NSObject <ASWebAuthenticationPresentationContextProviding>
@end

@implementation PvzpSignInAnchor

- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:(ASWebAuthenticationSession*)theSession
{
	(void)theSession;
	for (UIScene* aScene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![aScene isKindOfClass:[UIWindowScene class]])
			continue;
		for (UIWindow* aWindow in ((UIWindowScene*)aScene).windows)
		{
			if (aWindow.isKeyWindow)
				return aWindow;
		}
	}
	for (UIScene* aScene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([aScene isKindOfClass:[UIWindowScene class]] && ((UIWindowScene*)aScene).windows.count > 0)
			return ((UIWindowScene*)aScene).windows.firstObject;
	}
	return [UIWindow new];
}

@end

namespace
{

ASWebAuthenticationSession*	gSession = nil;
PvzpSignInAnchor*			gAnchor = nil;	// the session only holds it weakly
bool						gClosing = false;
bool						gCancelled = false;

}

namespace CloudSignInSheet
{

bool Open(const std::string& thePage)
{
	Close();
	NSString* aPage = [[NSString alloc] initWithBytes:thePage.data() length:thePage.size() encoding:NSUTF8StringEncoding];
	NSURL* anUrl = aPage != nil ? [NSURL URLWithString:aPage] : nil;
	if (anUrl == nil)
		return false;

	gCancelled = false;
	if (gAnchor == nil)
		gAnchor = [PvzpSignInAnchor new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	// No callback scheme: the page comes back to the loopback listener, never to the sheet. The initializer that
	// replaces this one needs iOS 17.4.
	ASWebAuthenticationSession* aSession = [[ASWebAuthenticationSession alloc] initWithURL:anUrl callbackURLScheme:nil
		completionHandler:^(NSURL* theCallback, NSError* theError)
	{
		(void)theCallback;
		(void)theError;
		// runs on the main thread, also after Close(); only a sheet the player closed counts as a cancel
		if (!gClosing)
			gCancelled = true;
		gSession = nil;
	}];
#pragma clang diagnostic pop
	aSession.presentationContextProvider = gAnchor;
	aSession.prefersEphemeralWebBrowserSession = NO;	// an account already signed in to in Safari can be picked
	if (![aSession start])
		return false;
	gSession = aSession;
	return true;
}

void Close()
{
	if (gSession == nil)
		return;
	gClosing = true;
	[gSession cancel];	// calls the completion handler right away
	gClosing = false;
	gSession = nil;
	gCancelled = false;
}

bool TakeCancelled()
{
	bool aCancelled = gCancelled;
	gCancelled = false;
	return aCancelled;
}

}

#endif
