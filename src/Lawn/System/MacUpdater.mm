/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local Mac app updater patch).
 */

#import <Foundation/Foundation.h>

#include "AppUpdater.h"

// The two methods of SPUStandardUpdaterController this file calls, declared here so it builds without Sparkle's headers.
@interface NSObject (PvzpSparkleUpdaterController)
- (instancetype)initWithStartingUpdater:(BOOL)theStartUpdater
						updaterDelegate:(id)theUpdaterDelegate
					 userDriverDelegate:(id)theUserDriverDelegate;
- (void)checkForUpdates:(id)theSender;
@end

namespace
{
id gController = nil;	// never released: it lives as long as the app
}

namespace AppUpdater
{

void Start()
{
	if (gController != nil)
		return;

	@autoreleasepool
	{
		NSBundle* aApp = [NSBundle mainBundle];
		if ([aApp objectForInfoDictionaryKey:@"SUFeedURL"] == nil)
			return;		// not an app made for updates (a bare binary, or a build without a feed)

		NSURL* aFrameworksUrl = [aApp privateFrameworksURL];
		if (aFrameworksUrl == nil)
			return;
		NSBundle* aSparkle = [NSBundle bundleWithURL:[aFrameworksUrl URLByAppendingPathComponent:@"Sparkle.framework"]];
		NSError* anError = nil;
		if (aSparkle == nil || ![aSparkle loadAndReturnError:&anError])
		{
			NSLog(@"Updater: could not load Sparkle.framework: %@", anError);
			return;
		}

		Class aControllerClass = NSClassFromString(@"SPUStandardUpdaterController");
		if (aControllerClass == nil)
		{
			NSLog(@"Updater: Sparkle.framework has no SPUStandardUpdaterController");
			return;
		}
		// Starting the updater schedules Sparkle's own background checks (SUEnableAutomaticChecks in Info.plist); when
		// one finds a newer version, Sparkle shows its window with the Install Update button.
		gController = [[aControllerClass alloc] initWithStartingUpdater:YES updaterDelegate:nil userDriverDelegate:nil];
	}
}

bool Available()
{
	return gController != nil;
}

void CheckNow()
{
	if (gController != nil)
		[gController checkForUpdates:nil];
}

}
