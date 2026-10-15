/**
 * @file luminousfloater.h
 * @brief Luminous's own window.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
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
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMINOUS_FLOATER_H
#define LUMINOUS_FLOATER_H

#include "llfloater.h"
#include "llframetimer.h"

class LLRenderTarget;

#include <map>
#include <set>
#include <vector>

class LLButton;
class LLImageRaw;
class LLViewerTexture;
class LLPanel;
class LLTabContainer;
class LuminousSideTabs;

/**
 * Luminous's window (floater_luminous.xml). Almost every widget in it is bound
 * to a setting and needs nothing from here; this class runs the presets and
 * camera positions, keeps the tick boxes of the effects whose amount is
 * their switch, and opens at the tab named in its key, if any.
 *
 * It is a sidebar, like Blender's: held against the right edge of the world
 * over the scene, as tall as the space between the toolbars, with its tabs
 * as a column on its right. Clicking the open tab folds it to that column;
 * clicking any tab opens it at that tab. It opens by itself at login,
 * folded, and never shows at the login screen.
 */
class LuminousFloater : public LLFloater
{
public:
    LuminousFloater(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override; ///< an effect's picture, without and with
    void draw() override;

    // Called from LLStartUp as the viewer reaches the world: the sidebar,
    // folded to its column of tabs.
    static void openAtLogin();

    // How much of the right edge of the world the sidebar covers, column and
    // open box, with a small gap: Firestorm's notification icons
    // (the chiclet bar) and its notice pop-ups stay left of it
    // (LLScreenChannelBase::getChannelRect). 0 while it is not shown.
    static S32 reservedRight();

    LLTabContainer* tabs() const { return mTabs; }
    std::string tabHint(LLPanel* panel) const;   // a tab's hint, for its button in the column
    bool isFolded() const { return mFolded; }
    // A tab in the column was clicked: the open one folds the sidebar, any
    // other opens it there.
    void onSideTab(S32 index);

private:
    void refreshShots(const std::string& select = std::string());
    void refreshTonemapper();
    void refreshLUTs();
    void refreshHSL();
    void refreshOptions();
    void onMaster(const LLSD& which);
    void onLogSlider(const LLSD& which);
    void refreshLogSliders();
    void onVectorSlider(const LLSD& which);
    void refreshVectorSliders();
    void onHSLChanged();
    void onLoadLUT();
    void onSaveShot();
    void onGoToShot();
    void onDeleteShot();
    void refreshSkies();   // the Sky menu: the region's, Luminous's, the user's own PBR skies
    void scanSkies();
    void refreshPresets(const std::string& select = std::string());
    void onSavePreset();
    void onLoadPreset();
    void onDeletePreset();
    void onShowTab(const LLSD& tab);
    void onEffectMode(size_t index);
    void refreshEffectSwitches();
    void refreshCapture();
    void fitTabs();
    void setFolded(bool folded);
    void dock();

    LLFrameTimer mRefreshTimer;
    // Each tab's rows as laid out, so that its scrolling panel can be fitted
    // to the rows shown when a disclosure opens or closes.
    std::map<LLView*, S32> mRowHeights;

    LLTabContainer* mTabs = nullptr;

    std::map<std::string, std::string> mTabHints; // tab panel name -> its hint, taken off the panel
    LLPanel* mBox = nullptr;
    LuminousSideTabs* mSideTabs = nullptr;
    bool mFolded = false;
    S32 mPauseShown = -1;      // the pause buttons' icon: 0 pause, 1 play again
    void refreshPauseButtons();
    S32 mOpenWidth = 0; // as laid out: the content and the column
    std::vector<LLHandle<LLView>> mTopBars;       // Firestorm's navigation and favourites bars
    std::vector<std::pair<LLHandle<LLButton>, S32>> mEffectButtons; // per EFFECT_MODES: the button, the state shown
    std::vector<LLView*> mPreviewViews;           // per EFFECT_PREVIEWS: the effect's name, or null
    static void reserveRight(S32 width);
};

/**
 * Luminous's render window: a render sent on from the render overlay's
 * Share (LuminousPictureView) shows here, with all of sending it to one of
 * Second Life's places -- where to, a texture's shape and size and its fee,
 * a caption or recipient, and Send.
 * Underneath it is Firestorm's own: the texture upload of its snapshot
 * window, the profile feed (LLWebProfile), Flickr and Primfeed's uploads
 * once signed in through their windows, and postcards.
 */
class LuminousRenderFloater : public LLFloater
{
public:
    LuminousRenderFloater(const LLSD& key);
    ~LuminousRenderFloater() override;
    bool postBuild() override;
    void draw() override;

    void onClose(bool app_quitting) override;

    /// Shows the picture, ready to send to destination (1 inventory,
    /// 2 profile feed, 3 Flickr, 4 Primfeed, 5 email).
    static void showPicture(const LLPointer<LLImageRaw>& picture, U32 destination);

private:
    void setPicture(const LLPointer<LLImageRaw>& picture, U32 destination);
    void refresh() override;
    void onSend();
    U32 destination() const;
    void fitToPanel();         // the window as tall as the shown place's rows
    /// The texture as it would be uploaded: its size and where the picture
    /// lands in it (cut: the part of the picture used).
    void textureShape(S32& tex_w, S32& tex_h, LLRect& picture_part) const;
    LLPointer<LLImageRaw> makeTexture() const;
    LLPointer<LLImageRaw> scaledToFit(S32 longest) const;

    LLPointer<LLImageRaw> mPicture;
    LLPointer<LLViewerTexture> mPreview;
    LLFrameTimer mRefreshTimer;
};

/**
 * The render, over the viewer. Rendering, the world as last drawn, frozen
 * over the interface, with how far the render is -- drawn between its
 * frames too (drawNow), which otherwise show nothing. Done, the picture at
 * full size: the scroll wheel zooms about the pointer up to 400%, 100%
 * being one picture pixel to one screen pixel; dragging moves it; a double
 * click goes between the whole picture and 100%.
 * Stop (or Esc) ends a render early, without a picture.
 * Share downloads it, opens it in Preview, or sends it on through the render
 * window; Close (or Esc) asks first if it was neither downloaded nor sent
 * on, for unless Render to disk is on the render is only in a temporary
 * file, removed on closing. The picture is a texture of its own at full
 * size -- not LLViewerTexture, whose sides must be powers of two.
 */
class LuminousPictureView : public LLFloater
{
public:
    LuminousPictureView(const LLSD& key);
    ~LuminousPictureView() override;
    bool postBuild() override;
    void draw() override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onClose(bool app_quitting) override;

    /// A render begins: the overlay, the world frozen as next drawn.
    static void beginRender();
    /// How far it is (0 to 1, or below 0 for not known); draw_now shows it
    /// at once, from inside a render that holds up the viewer's main loop.
    static void setProgress(F32 fraction, const std::string& text, bool draw_now);
    /// Done: the picture, at path (temporary: removed on closing);
    /// light_only: an EXR's, shown without the Look's tone.
    static void showResult(const LLPointer<LLImageRaw>& picture, const std::string& path, bool temporary, bool light_only);
    /// Done without a picture to show (saved to disk, failed or stopped).
    static void endRender();
    /// A note over the picture's top, while it renders and when done: a
    /// render smaller than asked, why, and what helps ("\n" between
    /// paragraphs). Cleared when the next render begins.
    static void setNote(const std::string& note);
    static bool rendering();
    /// Stop was asked for, with the Stop button or Esc: the render ends
    /// without a picture.
    static bool stopAsked();
    /// From inside the render, between its frames: the clicks and keys that
    /// came meanwhile, which the held-up main loop cannot handle -- a click
    /// on Stop or Esc stops it, the rest are dropped. Then stopAsked().
    static bool pollStop();

    /// The pipeline's last frame, before it is presented, as the still.
    static bool wantsStill();
    static void captureStill(LLRenderTarget* frame);

private:
    enum class State { Idle, Rendering, Done };
    static LuminousPictureView* instance();
    void setPicture(const LLPointer<LLImageRaw>& picture);
    void release();
    void requestClose();       // Close and Esc: asks first if the render would be lost
    void requestStop();        // Stop and Esc while rendering
    void discard();            // closes, the temporary file removed
    void onShare(const std::string& what);
    void fitToWindow();        // to the floaters' snap rect, the buttons placed
    void placeButtons(const LLRect& safe); // the part inside the toolbars, then placeOnPicture
    void placeOnPicture();     // Share, Close, Stop and the menu over the picture's bottom right
    LLRect pictureRect() const; // where the picture (or the still) is drawn
    LLRect onPicture() const;  // what of it shows inside the toolbars, or that part if too small
    void showButtons();
    void drawNote(const LLRect& on) const;
    LLRect area() const;       // where the picture is drawn: all of the overlay
    F32 fitScale() const;      // UI units per picture pixel, the whole picture
    F32 fullScale() const;     // ... one picture pixel to one screen pixel
    void zoomAbout(F32 scale, S32 x, S32 y);
    void clampCentre();

    State mState = State::Idle;
    F32 mProgress = -1.f;
    std::string mProgressText;
    std::string mNote;
    LLPointer<LLImageRaw> mPicture;
    std::string mPath;
    bool mTemporary = false;
    bool mKept = false;        // downloaded or sent on
    bool mLightOnly = false;
    bool mStopAsked = false;

    U32 mTexture = 0;
    S32 mWidth = 0;
    S32 mHeight = 0;
    F32 mScale = 0.f;          // UI units per picture pixel; 0 until shown
    F32 mCentreX = 0.f;        // the picture pixel at the area's centre
    F32 mCentreY = 0.f;
    bool mDragging = false;
    S32 mLastX = 0;
    S32 mLastY = 0;
    bool mRaising = false;     // brought to the front at the next idle
    LLRect mSafe;              // inside the toolbars, in the overlay's coordinates
    bool mRefitting = false;   // fitted to the window at the next idle
    LLRect mFittedTo;          // the snap rect last fitted to
};

#endif // LUMINOUS_FLOATER_H
