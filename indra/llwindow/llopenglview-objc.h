/**
 * @file llopenglview-objc.h
 * @brief Class interfaces for most of the Mac facing window functionality.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

#ifndef LLOpenGLView_H
#define LLOpenGLView_H

#import <Cocoa/Cocoa.h>
#import <IOKit/IOKitLib.h>
#import <CoreFoundation/CFBase.h>
#import <CoreFoundation/CFNumber.h>
#include <string>

@interface LLOpenGLView : NSView <NSTextInputClient>
{
    std::string mLastDraggedUrl;
    unsigned int mModifiers;
    float mMousePos[2];
    bool mHasMarkedText;
    unsigned int mMarkedTextLength;
    bool mMarkedTextAllowed;
    bool mSimulatedRightClick;
    // [Viewer Lab] The view's GL context (an NSOpenGLContext) and pixel format (an NSOpenGLPixelFormat), retained,
    // which NSOpenGLView kept. Typed id so this header names no GL type; only the context methods in
    // llopenglview-objc.mm use them.
    id mGLContext;
    id mGLPixelFormat;
    // [Viewer Lab] The golden hook's backing scale for a view with a CAMetalLayer; 0 is off.
    float mTestBackingScale;
}
- (id) initWithSamples:(NSUInteger)samples;
- (id) initWithSamples:(NSUInteger)samples andVsync:(BOOL)vsync;
- (id) initWithFrame:(NSRect)frame withSamples:(NSUInteger)samples andVsync:(BOOL)vsync;

- (void)commitCurrentPreedit;

// rebuildContext
// Destroys and recreates a context with the view's internal format set via setPixelFormat;
// Use this in event of needing to rebuild a context for whatever reason, without needing to assign a new pixel format.
- (BOOL) rebuildContext;

// [Viewer Lab] clearGLContext
// Clears the context's drawable and releases the context, as NSOpenGLView's did; removeGLView calls it.
- (void) clearGLContext;

// [Viewer Lab] createGLContextWithVsync:
// The view's GL context and pixel format; LLWindowMacOSX::createContext calls it through the bridge's
// createOpenGLContext when the GL backend creates the device. The Metal backend never calls it.
- (BOOL) createGLContextWithVsync:(BOOL)vsync;

// [Viewer Lab] setUseMetalLayer:
// Views made after this call with YES get a CAMetalLayer as their backing layer (makeBackingLayer); the bridge's
// setOpenGLViewMetalLayer calls it before the window makes its view.
+ (void) setUseMetalLayer:(BOOL)use;

// [Viewer Lab] llConvertSizeToBacking:, llConvertRectToBacking:, llConvertPointToBacking:
// The viewer's conversions to backing pixels. With a CAMetalLayer they scale by the backing scale with RenderHiDPI
// (or the test override) and by 1 without it, as GL's surface does; otherwise they are AppKit's convert*ToBacking: methods.
- (NSSize) llConvertSizeToBacking:(NSSize)size;
- (NSRect) llConvertRectToBacking:(NSRect)rect;
- (NSPoint) llConvertPointToBacking:(NSPoint)point;

// [Viewer Lab] setTestBackingScale:
// The golden hook's backing scale for a view with a CAMetalLayer, in place of the window's (0 is off), for a Mac
// with only a 1x display; then AppKit's calls on a screen change (viewDidChangeBackingProperties,
// windowDidChangeScreen:). Main thread. The bridge's setOpenGLViewTestBackingScale calls it.
- (void) setTestBackingScale:(float)scale;

- (unsigned long) getVramSize;

- (void) allowMarkedTextInput:(bool)allowed;

@end

@interface LLUserInputWindow : NSPanel

@end

@interface LLNonInlineTextView : NSTextView
{
    LLOpenGLView *glview;
    unichar mKeyPressed;
}

- (void) setGLView:(LLOpenGLView*)view;

@end

@interface LLNSWindow : NSWindow

@end

@interface NSScreen (PointConversion)

/*
 Returns the screen where the mouse resides
 */
+ (NSScreen *)currentScreenForMouseLocation;

@end

// [Viewer Lab] LLOpenGLView's context methods whose declarations name GL types.
#import "llopenglviewcontext-objc.h"

#endif
