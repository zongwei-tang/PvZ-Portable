/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * This file is part of PvZ-Portable.
 */

#include "MacIcon.h"
#import <AppKit/AppKit.h>

namespace MacIcon
{

void Apply(const std::string& thePicturePath)
{
	@autoreleasepool
	{
		NSString* aPath = [NSString stringWithUTF8String:thePicturePath.c_str()];
		NSFileManager* aFiles = [NSFileManager defaultManager];
		if (aPath == nil || ![aFiles fileExistsAtPath:aPath])
			return;
		NSImage* anImage = [[NSImage alloc] initWithContentsOfFile:aPath];
		if (anImage == nil || !anImage.isValid)
			return;

		// the Dock, while the game runs
		[NSApp setApplicationIconImage:anImage];

		// Finder, and the Dock after it: the custom icon of the bundle, kept in its "Icon\r" file. Set it when the bundle
		// has none (a new app, or one an update replaced) or the picture changed since.
		NSString* aBundle = [[NSBundle mainBundle] bundlePath];
		if (![aBundle hasSuffix:@".app"])
			return;		// the bare binary of a build folder
		NSString* aMarker = [aBundle stringByAppendingPathComponent:@"Icon\r"];
		NSDate* aPictureDate = [[aFiles attributesOfItemAtPath:aPath error:nil] fileModificationDate];
		NSDate* aMarkerDate = [[aFiles attributesOfItemAtPath:aMarker error:nil] fileModificationDate];
		if (aMarkerDate != nil && aPictureDate != nil && [aMarkerDate compare:aPictureDate] != NSOrderedAscending)
			return;
		[[NSWorkspace sharedWorkspace] setIcon:anImage forFile:aBundle options:0];
	}
}

}
