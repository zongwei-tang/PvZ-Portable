/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable (local cloud save patch).
 */

#if defined(PVZP_CLOUD_SAVE) && defined(PVZP_CLOUD_SIGN_IN_SHEET)

#import <UIKit/UIKit.h>

#include "CloudBackgroundTask.h"
#include <mutex>

#if !__has_feature(objc_arc)
#error CloudBackgroundTask.mm must be built with ARC (-fobjc-arc)
#endif

namespace
{

std::mutex				gMutex;
UIBackgroundTaskIdentifier	gTask = UIBackgroundTaskInvalid;

void EndLocked()
{
	if (gTask == UIBackgroundTaskInvalid)
		return;
	UIBackgroundTaskIdentifier aTask = gTask;
	gTask = UIBackgroundTaskInvalid;
	[UIApplication.sharedApplication endBackgroundTask:aTask];
}

}

namespace CloudBackgroundTask
{

void Begin()
{
	std::lock_guard aLock(gMutex);
	if (gTask != UIBackgroundTaskInvalid)
		return;
	// iOS calls the handler when the time is up: the upload is then given up, and tried again at the next start
	gTask = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Cloud save upload" expirationHandler:^
	{
		std::lock_guard aHandlerLock(gMutex);
		EndLocked();
	}];
}

void End()
{
	std::lock_guard aLock(gMutex);
	EndLocked();
}

}

#endif
