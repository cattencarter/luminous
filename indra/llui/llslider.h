/**
 * @file llslider.h
 * @brief A simple slider with no label.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLSLIDER_H
#define LL_LLSLIDER_H

#include "llf32uictrl.h"
#include "v4color.h"
#include "lluiimage.h"

class LLSlider : public LLF32UICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLF32UICtrl::Params>
    {
        Optional<std::string> orientation;

        Optional<LLUIColor> thumb_outline_color,
                            thumb_center_color;

        Optional<LLUIImage*>    thumb_image,
                                thumb_image_pressed,
                                thumb_image_disabled,
                                track_image_horizontal,
                                track_image_vertical,
                                track_highlight_horizontal_image,
                                track_highlight_vertical_image;

        Optional<CommitCallbackParam>   mouse_down_callback,
                                        mouse_up_callback;


        Params();
    };
protected:
    LLSlider(const Params&);
    friend class LLUICtrlFactory;
public:
    virtual ~LLSlider();
    void            setValue( F32 value, bool from_event = false );
    // overrides for LLF32UICtrl methods
    virtual void    setValue(const LLSD& value )    { setValue((F32)value.asReal(), true); }

    virtual void    setMinValue(const LLSD& min_value) { setMinValue((F32)min_value.asReal()); }
    virtual void    setMaxValue(const LLSD& max_value) { setMaxValue((F32)max_value.asReal()); }
    virtual void    setMinValue(F32 min_value) { LLF32UICtrl::setMinValue(min_value); updateThumbRect(); }
    virtual void    setMaxValue(F32 max_value) { LLF32UICtrl::setMaxValue(max_value); updateThumbRect(); }

    boost::signals2::connection setMouseDownCallback( const commit_signal_t::slot_type& cb );
    boost::signals2::connection setMouseUpCallback( const commit_signal_t::slot_type& cb );

    // <Luminous> Both off unless set, so other sliders behave as before.
    // A detent: a drag that crosses value, coming within pixels of it, holds
    // there exactly until the thumb is pulled more than pixels away, so a
    // neutral value is easy to find by dragging; a drag that comes from one
    // side without crossing gives the values next to it as any slider does.
    // Keys, the scroll wheel and typed values are not caught.
    void            setDetent(F32 value, S32 pixels);
    // The detent's decision for one drag position: offset, the thumb's
    // pixels from the detent; side, the side it was last on (-1, 1; 0
    // none yet) and held, whether it holds there, both kept between calls.
    // True: the value is the detent's.
    static bool     detentHolds(S32 offset, S32 pixels, S32& side, bool& held)
    {
        const S32 now_side = (offset > 0) - (offset < 0);
        const bool near = offset <= pixels && offset >= -pixels;
        bool holds = false;
        if (held && near)
        {
            holds = true; // still within reach: held
        }
        else if (held)
        {
            held = false; // pulled away: free again
        }
        else if (now_side == 0)
        {
            holds = true; // on the detent's own pixel
        }
        else if (near && side != 0 && now_side != side)
        {
            held = true; // crossed it within reach: caught
            holds = true;
        }
        if (now_side != 0)
        {
            side = now_side;
        }
        return holds;
    }
    // The value a double click (or a Ctrl-click, which otherwise goes to the
    // initial value) puts back, committed as a drag to it is. A value
    // outside the range is a bound control's own (such as one meaning "not
    // set"): it is written to the control as it is.
    void            setResetValue(F32 value);
    // Puts back the reset value; false if there is none.
    bool            resetValue();
    // </Luminous>

    virtual bool    handleHover(S32 x, S32 y, MASK mask);
    virtual bool    handleMouseUp(S32 x, S32 y, MASK mask);
    virtual bool    handleMouseDown(S32 x, S32 y, MASK mask);
    virtual bool    handleDoubleClick(S32 x, S32 y, MASK mask); // <Luminous/>
    virtual bool    handleKeyHere(KEY key, MASK mask);
    virtual bool    handleScrollWheel(S32 x, S32 y, S32 clicks);
    virtual void    draw();

private:
    void            setValueAndCommit(F32 value);
    void            updateThumbRect();
    // <Luminous>
    void            dragTo(S32 pos, S32 low_edge, S32 high_edge); // the thumb's centre along the track
    void            commitExactValue(F32 value); // not rounded to the increment: a detent's, a reset's

    F32             mDetentValue = 0.f;
    S32             mDetentPixels = 0;
    S32             mDetentSide = 0;      // the drag's side of the detent (detentHolds)
    bool            mDetentHeld = false;  // the drag holds at the detent
    bool            mHasResetValue = false;
    F32             mResetValue = 0.f;
    // </Luminous>

    bool            mVolumeSlider;
    S32             mMouseOffset;
    LLRect          mDragStartThumbRect;

    LLPointer<LLUIImage>    mThumbImage;
    LLPointer<LLUIImage>    mThumbImagePressed;
    LLPointer<LLUIImage>    mThumbImageDisabled;
    LLPointer<LLUIImage>    mTrackImageHorizontal;
    LLPointer<LLUIImage>    mTrackImageVertical;
    LLPointer<LLUIImage>    mTrackHighlightHorizontalImage;
    LLPointer<LLUIImage>    mTrackHighlightVerticalImage;

    const EOrientation  mOrientation;

    LLRect      mThumbRect;
    LLUIColor   mThumbOutlineColor;
    LLUIColor   mThumbCenterColor;

    commit_signal_t*    mMouseDownSignal;
    commit_signal_t*    mMouseUpSignal;
};

#endif  // LL_LLSLIDER_H
