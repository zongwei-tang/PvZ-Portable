/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local app updater patch).
 */

// Updates of the iOS app (see AppUpdater.h). The release of every tag carries altstore-source.json, the AltStore source
// that AltStore and SideStore install and update the app from; Info.plist names the one of the latest release
// (PVZPUpdateSourceURL). The game reads its newest build number and, when that is above its own CFBundleVersion, says
// so and opens AltStore or SideStore, where Update installs it over this one with the saves kept. Built with ARC.

#ifdef PVZP_IOS_UPDATER

#import <UIKit/UIKit.h>

#include "AppUpdater.h"

#if !__has_feature(objc_arc)
#error IosUpdater.mm must be built with ARC (-fobjc-arc)
#endif

namespace
{

NSString* const LAST_CHECK_KEY = @"PVZPLastUpdateCheck";
const NSTimeInterval AUTO_CHECK_INTERVAL = 20 * 60 * 60;	// about once a day, as the Mac app

NSURL*	gSourceUrl = nil;
NSURL*	gReleasesUrl = nil;
bool	gChecking = false;

NSString* InfoString(NSString* theKey)
{
	id aValue = [NSBundle.mainBundle objectForInfoDictionaryKey:theKey];
	return [aValue isKindOfClass:[NSString class]] && ((NSString*)aValue).length > 0 ? (NSString*)aValue : nil;
}

UIViewController* TopController()
{
	UIWindow* aWindow = nil;
	for (UIScene* aScene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![aScene isKindOfClass:[UIWindowScene class]])
			continue;
		for (UIWindow* aSceneWindow in ((UIWindowScene*)aScene).windows)
		{
			if (aSceneWindow.isKeyWindow || aWindow == nil)
				aWindow = aSceneWindow;
		}
	}
	UIViewController* aController = aWindow.rootViewController;
	while (aController.presentedViewController != nil)
		aController = aController.presentedViewController;
	return aController;
}

void OpenUrl(NSURL* theUrl)
{
	if (theUrl != nil)
		[UIApplication.sharedApplication openURL:theUrl options:@{} completionHandler:nil];
}

void Show(NSString* theTitle, NSString* theMessage, bool theOffersUpdate)
{
	UIViewController* aController = TopController();
	if (aController == nil)
		return;
	UIAlertController* anAlert = [UIAlertController alertControllerWithTitle:theTitle message:theMessage
		preferredStyle:UIAlertControllerStyleAlert];
	if (theOffersUpdate)
	{
		// the schemes are listed in LSApplicationQueriesSchemes, so canOpenURL can tell which of the two is installed
		NSURL* anAltStore = [NSURL URLWithString:@"altstore://"];
		NSURL* aSideStore = [NSURL URLWithString:@"sidestore://"];
		if ([UIApplication.sharedApplication canOpenURL:anAltStore])
			[anAlert addAction:[UIAlertAction actionWithTitle:@"Open AltStore" style:UIAlertActionStyleDefault
				handler:^(UIAlertAction*) { OpenUrl(anAltStore); }]];
		if ([UIApplication.sharedApplication canOpenURL:aSideStore])
			[anAlert addAction:[UIAlertAction actionWithTitle:@"Open SideStore" style:UIAlertActionStyleDefault
				handler:^(UIAlertAction*) { OpenUrl(aSideStore); }]];
		if (gReleasesUrl != nil)
			[anAlert addAction:[UIAlertAction actionWithTitle:@"Release Page" style:UIAlertActionStyleDefault
				handler:^(UIAlertAction*) { OpenUrl(gReleasesUrl); }]];
		[anAlert addAction:[UIAlertAction actionWithTitle:@"Later" style:UIAlertActionStyleCancel handler:nil]];
	}
	else
		[anAlert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
	[aController presentViewController:anAlert animated:YES completion:nil];
}

NSDictionary* FirstVersion(id theApp)
{
	NSArray* aVersions = [theApp isKindOfClass:[NSDictionary class]] ? theApp[@"versions"] : nil;
	if ([aVersions isKindOfClass:[NSArray class]] && aVersions.count > 0 && [aVersions[0] isKindOfClass:[NSDictionary class]])
		return aVersions[0];
	return nil;
}

// The newest version in an AltStore source: versions[0] of the app with this bundle identifier. A sideloading app may
// sign the app with its own identifier, the one of the source plus a suffix (such as the team ID), so a source of a
// single app is taken as this one's.
NSDictionary* NewestVersion(NSData* theData)
{
	id aSource = theData != nil ? [NSJSONSerialization JSONObjectWithData:theData options:0 error:nil] : nil;
	NSArray* anApps = [aSource isKindOfClass:[NSDictionary class]] ? aSource[@"apps"] : nil;
	if (![anApps isKindOfClass:[NSArray class]] || anApps.count == 0)
		return nil;
	NSString* anOwn = NSBundle.mainBundle.bundleIdentifier ?: @"";
	for (id anApp in anApps)
	{
		NSString* anId = [anApp isKindOfClass:[NSDictionary class]] ? anApp[@"bundleIdentifier"] : nil;
		if ([anId isKindOfClass:[NSString class]] && ([anOwn isEqualToString:anId] || [anOwn hasPrefix:[anId stringByAppendingString:@"."]]))
			return FirstVersion(anApp);
	}
	return anApps.count == 1 ? FirstVersion(anApps[0]) : nil;
}

long long NumberOf(id theValue)
{
	if ([theValue isKindOfClass:[NSNumber class]])
		return ((NSNumber*)theValue).longLongValue;
	if ([theValue isKindOfClass:[NSString class]])
		return ((NSString*)theValue).longLongValue;
	return 0;
}

// theQuiet: only an update is shown, not "up to date" or an error (the check at start)
void Check(bool theQuiet)
{
	if (gSourceUrl == nil || gChecking)
		return;
	gChecking = true;
	[NSUserDefaults.standardUserDefaults setObject:[NSDate date] forKey:LAST_CHECK_KEY];

	NSURLRequest* aRequest = [NSURLRequest requestWithURL:gSourceUrl cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
		timeoutInterval:30];
	[[NSURLSession.sharedSession dataTaskWithRequest:aRequest
		completionHandler:^(NSData* theData, NSURLResponse* theResponse, NSError* theError)
	{
		NSInteger aStatus = [theResponse isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)theResponse).statusCode : 0;
		NSDictionary* aNewest = theError == nil && aStatus == 200 ? NewestVersion(theData) : nil;
		dispatch_async(dispatch_get_main_queue(), ^
		{
			gChecking = false;
			NSString* aHave = InfoString(@"CFBundleShortVersionString") ?: @"?";
			long long aHaveBuild = NumberOf([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]);
			if (aNewest == nil)
			{
				if (!theQuiet)
					Show(@"Check Updates", theError != nil ? [NSString stringWithFormat:@"Could not look for updates: %@",
						theError.localizedDescription] : @"Could not read the list of releases.", false);
				return;
			}
			long long aBuild = NumberOf(aNewest[@"buildVersion"]);
			id aVersion = aNewest[@"version"];
			NSString* aName = [aVersion isKindOfClass:[NSString class]] ? aVersion : @"?";
			if (aBuild > aHaveBuild)
				Show(@"Update Available", [NSString stringWithFormat:@"Version %@ (build %lld) is out, this is %@ (build %lld).\n\n"
					"Open AltStore or SideStore and tap Update next to PvZ Portable. Your saves stay.", aName, aBuild, aHave, aHaveBuild], true);
			else if (!theQuiet)
				Show(@"Check Updates", [NSString stringWithFormat:@"This is the newest version, %@ (build %lld).", aHave, aHaveBuild], false);
		});
	}] resume];
}

}

namespace AppUpdater
{

void Start()
{
	if (gSourceUrl != nil)
		return;
	NSString* aSource = InfoString(@"PVZPUpdateSourceURL");
	NSURL* aSourceUrl = aSource != nil ? [NSURL URLWithString:aSource] : nil;
	if (aSourceUrl == nil || ![aSourceUrl.scheme isEqualToString:@"https"])
		return;		// a build without a release feed
	gSourceUrl = aSourceUrl;
	NSString* aReleases = InfoString(@"PVZPReleasePageURL");
	gReleasesUrl = aReleases != nil ? [NSURL URLWithString:aReleases] : nil;

	NSDate* aLast = [NSUserDefaults.standardUserDefaults objectForKey:LAST_CHECK_KEY];
	if (![aLast isKindOfClass:[NSDate class]] || -[aLast timeIntervalSinceNow] >= AUTO_CHECK_INTERVAL)
		Check(true);
}

bool Available()
{
	return gSourceUrl != nil;
}

void CheckNow()
{
	Check(false);
}

}

#endif
