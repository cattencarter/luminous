/**
 * @file llappviewermacosx-objc.mm
 * @brief Functions related to LLAppViewerMacOSX that must be expressed in obj-c
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#if !defined LL_DARWIN
    #error "Use only with macOS"
#endif

#import <Cocoa/Cocoa.h>
#include <iostream>

#include "llappviewermacosx-objc.h"

void force_ns_sxeption()
{
    NSException *exception = [NSException exceptionWithName:@"Forced NSException" reason:nullptr userInfo:nullptr];
    @throw exception;
}

// <Luminous> Sharing a picture through another app or a web page.

std::string luminousPicturesFolder()
{
    @autoreleasepool {
        NSArray* folders = NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES);
        NSString* folder = [folders count] > 0 ? [folders objectAtIndex:0] : nil;
        return folder ? std::string([folder fileSystemRepresentation]) : std::string();
    }
}

void luminousRevealInFinder(const std::string& path)
{
    @autoreleasepool {
        NSString* file = [NSString stringWithUTF8String:path.c_str()];
        if (file)
        {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:[NSArray arrayWithObject:[NSURL fileURLWithPath:file]]];
        }
    }
}

// Makes the PNG of a JPEG on the clipboard from the JPEG's bytes when an app
// asks for it as it pastes, so that no time goes on it when nothing is
// pasted; the bytes are let go once the clipboard holds something else.
@interface LuminousPictureProvider : NSObject <NSPasteboardItemDataProvider>
{
    NSData* mBytes;
}
- (instancetype)initWithBytes:(NSData*)bytes;
@end

@implementation LuminousPictureProvider
- (instancetype)initWithBytes:(NSData*)bytes
{
    if ((self = [super init]))
    {
        mBytes = [bytes retain];
    }
    return self;
}

- (void)dealloc
{
    [mBytes release];
    [super dealloc];
}

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type
{
    @autoreleasepool {
        if (!mBytes || ![type isEqualToString:NSPasteboardTypePNG])
        {
            return;
        }
        NSBitmapImageRep* picture = [NSBitmapImageRep imageRepWithData:mBytes];
        NSData* data = picture ? [picture representationUsingType:NSBitmapImageFileTypePNG properties:[NSDictionary dictionary]] : nil;
        if (data)
        {
            [item setData:data forType:type];
        }
    }
}

- (void)pasteboardFinishedWithDataProvider:(NSPasteboard*)pasteboard
{
    [mBytes release];
    mBytes = nil;
}
@end

namespace
{
    // the provider of the picture last put on the clipboard, kept until the next
    LuminousPictureProvider* sPictureProvider = nil;
}

bool luminousCopyPictureFileToClipboard(const std::string& path)
{
    @autoreleasepool {
        NSString* file = [NSString stringWithUTF8String:path.c_str()];
        NSData* bytes = file ? [NSData dataWithContentsOfFile:file] : nil;
        if (!bytes)
        {
            return false;
        }
        // A PNG as it is; a JPEG as it is, and as PNG on asking (web
        // browsers paste only PNG and TIFF pictures). The clipboard is
        // cleared only once the new item is made.
        const bool png = [[[file pathExtension] lowercaseString] isEqualToString:@"png"];
        NSPasteboardItem* item = [[[NSPasteboardItem alloc] init] autorelease];
        if (![item setData:bytes forType:(png ? NSPasteboardTypePNG : @"public.jpeg")])
        {
            return false;
        }
        LuminousPictureProvider* provider = nil;
        if (!png)
        {
            provider = [[LuminousPictureProvider alloc] initWithBytes:bytes];
            if (![item setDataProvider:provider forTypes:[NSArray arrayWithObject:NSPasteboardTypePNG]])
            {
                [provider release];
                return false;
            }
        }
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        [sPictureProvider release];
        sPictureProvider = provider;
        return [board writeObjects:[NSArray arrayWithObject:item]];
    }
}
// </Luminous>
