/**
 * @file luminousfloater.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousfloater.h"

#include "llcombobox.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "llfilepicker.h"
#include "llviewermenufile.h"
#include "llnotificationsutil.h"
#include "lltabcontainer.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llrootview.h"
#include "llwindow.h"
#include "llgl.h"
#include "lllayoutstack.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llui.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llchannelmanager.h"
#include "llstartup.h"
#include "lltooltip.h"
#include "lliconctrl.h"
#include "llrender2dutils.h"
#include "llsliderctrl.h"
#include "luminousphotomode.h"
#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentui.h"
#include "llcheckboxctrl.h"
#include "llfilesystem.h"
#include "llflickrconnect.h"
#include "exoflickrauth.h"
#include "llfloaterperms.h"
#include "llimagej2c.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "lllineeditor.h"
#include "llpostcard.h"
#include "llslurl.h"
#include "lltexteditor.h"
#include "llviewerassetupload.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llwebprofile.h"
#include "exoflickr.h"
#include "fsprimfeedauth.h"
#include "fsprimfeedconnect.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "luminoushdrsnapshot.h"
#include "lldirpicker.h"
#include "llrendertarget.h"
#include "llmodaldialog.h"
#include "llvertexbuffer.h"
#include "pipeline.h"


#include <map>

namespace
{
    // Each effect's button: off, live, or only in the rendered picture. An
    // effect is switched by a setting of its own -- on or off, or an amount
    // whose 0 is off -- and kept for the picture by its
    // LuminousPhoto*RenderOnly where it can be; Firestorm's depth of field
    // cannot, and has two states. A click goes off, live, render only, off.
    // Turning an amount on brings back the amount it had, or a start that
    // shows; turning it off keeps the amount for next time.
    enum class EffectState { Off, Live, Render };
    struct EffectMode
    {
        const char* button;       // "<effect>_mode"
        const char* on;           // the switch, or nullptr for an amount
        const char* amount;       // an amount whose 0 is off, or nullptr
        const char* slider;       // the amount's slider: enabled while on
        F32 start;
        const char* render_only;  // nullptr: off and live only, or off and render only (always_render)
        bool always_render = false; // never live: off or in the rendered picture only
        const char* needs = nullptr; // a Firestorm switch it builds on: on and off with it
    };
    const EffectMode EFFECT_MODES[] = {
        { "volumetric_mode",   "LuminousPhotoVolumetricLight",     nullptr, nullptr, 0.f, "LuminousPhotoVolumetricLightRenderOnly" },
        { "beams_mode",        "LuminousPhotoProjectorBeams",      nullptr, nullptr, 0.f, "LuminousPhotoProjectorBeamsRenderOnly" },
        { "shafts_mode",       "LuminousPhotoSunShafts",           nullptr, nullptr, 0.f, "LuminousPhotoSunShaftsRenderOnly" },
        { "soft_shadows_mode", "LuminousPhotoSoftShadows",         nullptr, nullptr, 0.f, "LuminousPhotoSoftShadowsRenderOnly" },
        // GTAO is drawn in Firestorm's ambient occlusion pass, so its button
        // switches that too: all of AO is set here, in one place.
        { "gtao_mode",         "LuminousPhotoAO",                  nullptr, nullptr, 0.f, "LuminousPhotoAORenderOnly", false, "RenderDeferredSSAO" },
        { "contact_mode",      "LuminousPhotoContactShadows",      nullptr, nullptr, 0.f, "LuminousPhotoContactShadowsRenderOnly" },
        // Off or render only, never live: at the live view's sample counts
        // they look rougher than in the picture.
        { "ssr_mode",          "LuminousPhotoSSR",                 nullptr, nullptr, 0.f, nullptr, true },
        // off or only in the render: live, worn alpha is sorted only by
        // distance, which can show the scalp through hair at a parting
        { "alpha_sort_mode",   "LuminousPhotoAlphaSort",           nullptr, nullptr, 0.f, nullptr, true },
        { "dof_mode",          "RenderDepthOfField",               nullptr, nullptr, 0.f, nullptr },
        { "lens_blur_mode",    "LuminousPhotoDoF",                 nullptr, nullptr, 0.f, "LuminousPhotoDoFRenderOnly" },
        // Firestorm's glow and sharpening: drawn after the frame is toned, so
        // never kept for renders.
        { "object_glow_mode",  "RenderGlow",                       nullptr, nullptr, 0.f, nullptr },
        { "vignette_mode",     "LuminousPhotoVignette",            nullptr, nullptr, 0.f, "LuminousPhotoVignetteRenderOnly" },
        { "bloom_mode",        "LuminousPhotoBloom",               nullptr, nullptr, 0.f, "LuminousPhotoBloomRenderOnly" },
        { "ca_mode",           "LuminousPhotoChromaticAberration", nullptr, nullptr, 0.f, "LuminousPhotoChromaticAberrationRenderOnly" },
        { "halation_mode",   nullptr, "LuminousPhotoHalation",   "luminousphotohalation_amount",   0.25f, "LuminousPhotoHalationRenderOnly" },
        { "anamorphic_mode", nullptr, "LuminousPhotoAnamorphic", "luminousphotoanamorphic_amount", 0.3f,  "LuminousPhotoAnamorphicRenderOnly" },
        { "lens_flare_mode", nullptr, "LuminousPhotoLensFlare",  "luminousphotolensflare_amount",  0.15f, "LuminousPhotoLensFlareRenderOnly" },
        { "distortion_mode", nullptr, "LuminousPhotoDistortion", "luminousphotodistortion_amount", 0.15f, "LuminousPhotoDistortionRenderOnly" },
        { "grain_mode",      nullptr, "LuminousPhotoGrain",      "grain_amount",                   0.25f, "LuminousPhotoGrainRenderOnly" },
        { "clarity_mode",    nullptr, "LuminousPhotoClarity",    "look_clarity",                   0.2f,  "LuminousPhotoClarityRenderOnly" },
        { "sharpen_mode",    nullptr, "RenderCASSharpness",      "look_sharpening",                0.3f,  nullptr },
    };

    bool effectOn(const EffectMode& effect)
    {
        if (effect.needs && !gSavedSettings.getBOOL(effect.needs))
        {
            return false;
        }
        return effect.on ? gSavedSettings.getBOOL(effect.on) : gSavedSettings.getF32(effect.amount) != 0.f;
    }

    EffectState effectState(const EffectMode& effect)
    {
        if (!effectOn(effect))
        {
            return EffectState::Off;
        }
        if (effect.always_render)
        {
            return EffectState::Render;
        }
        return effect.render_only && gSavedSettings.getBOOL(effect.render_only) ? EffectState::Render : EffectState::Live;
    }

    std::map<std::string, F32> sEffectKept; // the amount an effect had when it was unticked

    // An effect's hint with a picture of it, without and with, so it shows
    // what the effect does even where the light at hand would not (lens
    // flare at night, projector beams in sunshine): the effect's name, the
    // picture, then what it does, and points only where there are separate
    // things to say. Shown for the effect's name; the picture is cropped
    // from two renders of the same view, without and with.
    struct EffectPreview
    {
        const char* widget;  // the effect's name
        const char* image;   // the picture, textures.xml
        const char* summary; // what it does
        std::vector<const char*> points;
        const char* title = nullptr; // when the widget is not a text (a slider's own label)
        S32 label_width = 0;         // and then only over its label
    };
    const EffectPreview EFFECT_PREVIEWS[] = {
        // Scene
        { "volumetric_name", "Luminous_Preview_Volumetric",
          "Sunlight glowing in the air, with beams where things cast shadows.",
          { "Shows most looking toward the sun, with something in front of it.",
            "Needs Shadows on in Graphics preferences." } },
        { "beams_name", "Luminous_Preview_Beams",
          "Beams in the air from projector lights, brightest at the lamp.",
          { "Needs a projector shining into view; best in the dark." } },
        { "shafts_name", "Luminous_Preview_Shafts",
          "Rays fanning out from the sun or moon.",
          { "Needs the sun in or near the picture, partly hidden by leaves, a roof or the like." } },
        { "soft_shadows_name", "Luminous_Preview_SoftShadows",
          "Shadows sharp where things touch and softer further away, as real ones.",
          { "Needs Shadows on in Graphics preferences." } },
        { "gtao_name", "Luminous_Preview_AO",
          "Soft darkening in creases, corners and where things meet.",
          { "Shows most in shade and overcast light." },
          "GTAO (ambient occlusion)" },
        { "contact_name", "Luminous_Preview_Contact",
          "Small, sharp shadows where things touch: fingers on skin, feet on the floor.",
          { "Without them a lit edge can show where something rests on a surface, as under her hip here.",
            "Needs Shadows on in Graphics preferences." } },
        { "ssr_name", "Luminous_Preview_SSR",
          "Cleaner reflections in shiny floors and surfaces.",
          { "Without it you get Second Life's: here her feet barely show in the floor.",
            "Needs Screen Space Reflections on in Graphics preferences." } },
        { "alpha_sort_name", "Luminous_Preview_AlphaSort",
          "See-through hair and eyelashes drawn in their place among leaves and other see-through things.",
          { "Without it you get Second Life's: here the sky shows through her hair as a pale sheet.",
            "Only in the rendered picture: live you see Second Life's." } },
        // Tone
        { "look_clarity", "Luminous_Preview_Clarity",
          "Right brings out texture in fabric, wood and skin; left softens it for a gentle portrait.",
          {},
          "Clarity", 92 },
        // Effects
        { "dof_name", "Luminous_Preview_DoF",
          "Blurs what is nearer or further than the focus, as a lens does. With Enhance it blurs "
          "as a real lens: lights open into discs, and the background never bleeds onto a sharp subject.",
          { "Enhance is under the arrow.",
            "Alt-click to focus." } },
        { "vignette_name", "Luminous_Preview_Vignette",
          "Darker corners, as from a lens.", {} },
        { "bloom_name", "Luminous_Preview_Bloom",
          "A soft glow round bright lights: lamps, windows, the sun on water.",
          { "Shows only where something is very bright." } },
        { "ca_name", "Luminous_Preview_Aberration",
          "Red and blue fringes toward the picture's edges, as from a simple lens.",
          { "Shows on sharp edges near the corners." } },
        { "halation_name", "Luminous_Preview_Halation",
          "A warm red-orange glow round very bright edges, as on film.",
          { "Shows where very bright light meets dark: a figure against the sun, lit panels in a "
            "dark room." } },
        { "anamorphic_name", "Luminous_Preview_Anamorphic",
          "Streaks through bright lights, as an anamorphic lens draws them.",
          { "Only very bright lights streak: the sun, its glints, lamps at night.",
            "Blue as the classic lenses draw them; another colour under the arrow." } },
        { "lens_flare_name", "Luminous_Preview_LensFlare",
          "Flare in the lens from bright lights: ghosts, a halo, a starburst.",
          { "Needs the sun or very bright lights in view.",
            "Subtle at 0.1 to 0.2." } },
        { "distortion_name", "Luminous_Preview_Distortion",
          "Bends straight lines: right bows them out (wide lens), left pulls them in (long lens).",
          { "Shows most on straight lines near the edges, such as a horizon." } },
        { "grain_name", "Luminous_Preview_Grain",
          "Film grain.",
          { "Easiest to see on smooth areas such as sky." } },
    };

    // The hint with its picture: the name in bold, a line, the picture, a
    // line, the text. The picture is made at twice the size it is drawn, for
    // Retina screens.
    class LuminousPreviewToolTip : public LLToolTip
    {
    public:
        LuminousPreviewToolTip(const LLToolTip::Params& p)
            : LLToolTip(p)
        {
            mMaxWidth = llmax(mMaxWidth, WIDTH);
        }

        void initFromParams(const LLToolTip::Params& p) override
        {
            LLToolTip::initFromParams(p);
            LLUIImagePtr image = LLUI::getUIImage(p.create_params()["image"].asString());
            if (!image)
            {
                return;
            }
            // every picture is 960 x 270, drawn at half (sized here, not from
            // the image, which may not have loaded yet)
            const S32 width = WIDTH;
            const S32 height = HEIGHT;
            const S32 GAP = 10;

            // From the bottom: the text, a line, the picture, a line, the
            // name -- each held to the bottom, or growing the tooltip to fit
            // them would carry those held to the top up out of it.
            const S32 text_h = mTextBox->getTextPixelHeight();
            mTextBox->setFollows(FOLLOWS_LEFT | FOLLOWS_BOTTOM);
            mTextBox->setRect(LLRect(MARGIN, MARGIN + text_h, MARGIN + width, MARGIN));
            S32 y = MARGIN + text_h + GAP;
            mLines[0] = y;
            y += GAP;
            LLIconCtrl::Params icon;
            icon.name("luminous_preview");
            icon.image(image);
            icon.mouse_opaque(false);
            icon.follows.flags(FOLLOWS_LEFT | FOLLOWS_BOTTOM);
            icon.rect(LLRect(MARGIN, y + height, MARGIN + width, y));
            addChild(LLUICtrlFactory::create<LLIconCtrl>(icon));
            y += height + GAP;
            mLines[1] = y;
            y += GAP - 2;
            const LLFontGL* font = LLFontGL::getFontSansSerifBold();
            const S32 title_h = (S32)font->getLineHeight() + 2;
            LLTextBox::Params title;
            title.name("luminous_preview_title");
            title.font(font);
            title.text_color(p.text_color);
            title.mouse_opaque(false);
            title.bg_visible(false);
            title.follows.flags(FOLLOWS_LEFT | FOLLOWS_BOTTOM);
            title.rect(LLRect(MARGIN, y + title_h, MARGIN + width, y));
            LLTextBox* title_box = LLUICtrlFactory::create<LLTextBox>(title);
            title_box->setText(p.create_params()["title"].asString());
            addChild(title_box);
            mLineWidth = width;
            // the same margin all round, wider than a plain hint's
            setShape(LLRect(0, y + title_h + MARGIN - 2, MARGIN + width + MARGIN, 0));
        }

        void draw() override
        {
            LLToolTip::draw();
            if (mLineWidth > 0)
            {
                const LLColor4 colour(1.f, 1.f, 1.f, 0.18f * getDrawContext().mAlpha);
                for (S32 y : mLines)
                {
                    gl_line_2d(MARGIN, y, MARGIN + mLineWidth, y, colour);
                }
            }
        }

    private:
        static constexpr S32 MARGIN = 9;
        static constexpr S32 WIDTH = 480;
        static constexpr S32 HEIGHT = 135;
        S32 mLines[2] = { 0, 0 };
        S32 mLineWidth = 0;
    };

    // The box's title row above the tab container, and a tab's footer below
    // its rows (reset, Render image), as laid out in floater_luminous.xml.
    constexpr S32 BOX_TITLE_HEIGHT = 26;
    constexpr S32 RESERVE_GAP = 4;  // between the sidebar and the icons or pop-ups left of it
    constexpr S32 FOOTER_HEIGHT = 44;

    // The tabs, by their panels' names.
    const char* const TABS[] = { "luminous_light", "luminous_look", "luminous_colour",
                                 "luminous_lens", "luminous_snapshot", "luminous_presets", "luminous_graphics" };

    // What each snapshot format is, in the format menu's tooltip, by the
    // setting's value.
    const char* const FORMAT_TEXT[] = {
        "PNG: for sharing. Slow to save at large sizes.",
        "16-bit TIFF: for heavy editing.",
        "EXR: for HDR editing.",
        "JPEG: high quality, small and quick to save.",
        "BMP: the quickest to save, and large.",
    };
}

// The sidebar's column, always there on the right edge: the tabs, each label
// turned to read from top to bottom as Blender draws its sidebar's tabs,
// Presets among them as a floppy disk, and below them, set a little apart,
// Settings as a gear. The tab container's own row is hidden; this draws its
// panels' labels and selects them.
class LuminousSideTabs : public LLView
{
public:
    static constexpr S32 WIDTH = 24;    // the column
    static constexpr S32 PAD = 10;      // above and below a label
    static constexpr S32 GAP = 2;       // between tabs
    static constexpr S32 TOP = 4;       // inside the column's background, top and bottom
    static constexpr S32 ICON = 16;     // an icon tab's glyph
    static constexpr S32 ICON_GAP = 10; // above the settings gear

    // The tabs drawn as an icon rather than a turned label: their image's
    // name, or nullptr.
    static const char* iconOf(LLPanel* panel)
    {
        if (!panel)
        {
            return nullptr;
        }
        if (panel->getName() == "luminous_presets")
        {
            return "Luminous_Presets";
        }
        return panel->getName() == "luminous_graphics" ? "Icon_Gear" : nullptr;
    }

    LuminousSideTabs(const LLView::Params& p, LuminousFloater* owner)
        : LLView(p), mOwner(owner)
    {
    }

    // How tall the column is with everything in it.
    S32 itemsHeight() const
    {
        std::vector<LLRect> rects;
        layout(rects, 10000);
        return rects.empty() ? 0 : 10000 - rects.back().mBottom + TOP;
    }

    void draw() override
    {
        LLTabContainer* tabs = mOwner->tabs();
        if (!tabs)
        {
            return;
        }
        layout(mRects, getRect().getHeight());
        if (mRects.empty())
        {
            return;
        }
        if (LLUIImagePtr back = LLUI::getUIImage("Window_NoTitle_Foreground"))
        {
            back->draw(LLRect(0, getRect().getHeight(), getRect().getWidth(), mRects.back().mBottom - TOP));
        }
        static LLUIColor label_colour = LLUIColorTable::instance().getColor("ButtonLabelColor");
        static LLUIColor open_colour = LLUIColorTable::instance().getColor("ButtonLabelSelectedColor");
        const S32 open = mOwner->isFolded() ? -1 : tabs->getCurrentPanelIndex();
        for (S32 i = 0; i < (S32)mRects.size(); ++i)
        {
            const LLRect& r = mRects[i];
            LLUIImagePtr image = LLUI::getUIImage(i == open ? "PushButton_Selected"
                                                  : (i == mHover ? "PushButton_Over" : "PushButton_Off"));
            if (image)
            {
                image->draw(r);
            }
            LLPanel* panel = tabs->getPanelByIndex(i);
            const LLColor4 colour = i == open ? open_colour.get() : label_colour.get();
            if (const char* icon = iconOf(panel))
            {
                if (LLUIImagePtr image = LLUI::getUIImage(icon))
                {
                    image->draw(r.getCenterX() - ICON / 2, r.getCenterY() - ICON / 2, ICON, ICON, colour);
                }
            }
            else
            {
                drawTurnedLabel(panel->getLabel(), r, colour);
            }
        }
    }

    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        S32 item;
        if (itemAt(x, y, item))
        {
            make_ui_sound("UISndClick");
            mOwner->onSideTab(item);
        }
        return true; // the column is the sidebar's, never the world's
    }

    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        S32 item;
        mHover = itemAt(x, y, item) ? item : -2;
        getWindow()->setCursor(UI_CURSOR_ARROW);
        return true;
    }

    void onMouseLeave(S32 x, S32 y, MASK mask) override
    {
        mHover = -2;
    }

    bool handleToolTip(S32 x, S32 y, MASK mask) override
    {
        S32 item;
        if (!itemAt(x, y, item))
        {
            return false;
        }
        std::string tip;
        if (LLTabContainer* tabs = mOwner->tabs())
        {
            LLPanel* panel = tabs->getPanelByIndex(item);
            const std::string hint = mOwner->tabHint(panel);
            tip = hint.empty() ? panel->getLabel() : panel->getLabel() + ": " + hint;
        }
        LLRect screen;
        localRectToScreen(mRects[item], &screen);
        LLToolTipMgr::instance().show(LLToolTip::Params().message(tip).sticky_rect(screen));
        return true;
    }

private:
    // A tab per panel, from the top of a column `height` tall.
    void layout(std::vector<LLRect>& rects, S32 height) const
    {
        rects.clear();
        LLTabContainer* tabs = mOwner->tabs();
        if (!tabs)
        {
            return;
        }
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        S32 top = height - TOP;
        for (S32 i = 0; i < tabs->getTabCount(); ++i)
        {
            LLPanel* panel = tabs->getPanelByIndex(i);
            S32 h = ll_round(font->getWidthF32(panel->getLabel())) + 2 * PAD;
            if (iconOf(panel))
            {
                h = WIDTH - 2;             // square: the icon alone
            }
            if (panel->getName() == "luminous_graphics")
            {
                top -= ICON_GAP - GAP;     // set a little apart from the tabs above
            }
            rects.emplace_back(1, top, WIDTH - 1, top - h);
            top -= h + GAP;
        }
    }

    bool itemAt(S32 x, S32 y, S32& item)
    {
        layout(mRects, getRect().getHeight());
        for (S32 i = 0; i < (S32)mRects.size(); ++i)
        {
            if (mRects[i].pointInRect(x, y))
            {
                item = i;
                return true;
            }
        }
        return false;
    }

    // The font places its glyphs in window pixels from its own origin, so the
    // label is turned on the modelview matrix about the tab's centre in window
    // pixels. A quarter turn keeps the glyphs on whole pixels.
    static void drawTurnedLabel(const std::string& label, const LLRect& r, const LLColor4& colour)
    {
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        const F32 cx = (LLFontGL::sCurOrigin.mX + r.getCenterX()) * LLFontGL::sScaleX;
        const F32 cy = (LLFontGL::sCurOrigin.mY + r.getCenterY()) * LLFontGL::sScaleY;
        gGL.flush();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();
        gGL.translatef(cx, cy, 0.f);
        gGL.rotatef(-90.f, 0.f, 0.f, 1.f);
        gGL.translatef(-cx, -cy, 0.f);
        font->renderUTF8(label, 0, (F32)r.getCenterX(), (F32)r.getCenterY(), colour,
                         LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        gGL.flush();
        gGL.popMatrix();
    }

    LuminousFloater* mOwner;
    std::vector<LLRect> mRects;
    S32 mHover = -2; // -1 is the switch
};

LuminousFloater::LuminousFloater(const LLSD& key)
    : LLFloater(key)
{
    mCommitCallbackRegistrar.add("Luminous.LoadLUT", boost::bind(&LuminousFloater::onLoadLUT, this));
    mCommitCallbackRegistrar.add("Luminous.HSL.Range", boost::bind(&LuminousFloater::refreshHSL, this));
    mCommitCallbackRegistrar.add("Luminous.Master", boost::bind(&LuminousFloater::onMaster, this, _2));
    mCommitCallbackRegistrar.add("Luminous.LogSlider", boost::bind(&LuminousFloater::onLogSlider, this, _2));
    mCommitCallbackRegistrar.add("Luminous.VectorSlider", boost::bind(&LuminousFloater::onVectorSlider, this, _2));
    mCommitCallbackRegistrar.add("Luminous.HSL.Change", boost::bind(&LuminousFloater::onHSLChanged, this));
    mCommitCallbackRegistrar.add("Luminous.Shot.Save", boost::bind(&LuminousFloater::onSaveShot, this));
    mCommitCallbackRegistrar.add("Luminous.Shot.Go", boost::bind(&LuminousFloater::onGoToShot, this));
    mCommitCallbackRegistrar.add("Luminous.Shot.Delete", boost::bind(&LuminousFloater::onDeleteShot, this));
    mCommitCallbackRegistrar.add("Luminous.Preset.Save", boost::bind(&LuminousFloater::onSavePreset, this));
    mCommitCallbackRegistrar.add("Luminous.Preset.Load", boost::bind(&LuminousFloater::onLoadPreset, this));
    mCommitCallbackRegistrar.add("Luminous.Preset.Delete", boost::bind(&LuminousFloater::onDeletePreset, this));
    mCommitCallbackRegistrar.add("Luminous.ShowTab", boost::bind(&LuminousFloater::onShowTab, this, _2));
    mCommitCallbackRegistrar.add("Luminous.SidebarClose", boost::bind(&LuminousFloater::setFolded, this, true));
}

bool LuminousFloater::postBuild()
{
    // The skies: the region's, Luminous's, then the user's own PBR skies
    // from inventory, asked about again each time the menu opens
    if (LLComboBox* skies = findChild<LLComboBox>("env_sky"))
    {
        refreshSkies();
        skies->setPrearrangeCallback([this](LLUICtrl*, const LLSD&) { scanSkies(); refreshSkies(); });
        scanSkies();
    }
    // The styles, Luminous's own
    if (LLComboBox* styles = findChild<LLComboBox>("style_names"))
    {
        styles->removeall();
        for (const std::string& name : LuminousPhotoMode::styleNames())
        {
            styles->add(name, LLSD(name));
        }
        styles->setValue(gSavedSettings.getString("LuminousStyle"));
    }
    refreshShots();
    refreshPresets();
    refreshLUTs();
    refreshHSL();
    refreshOptions();
    refreshLogSliders();
    refreshVectorSliders();
    for (const EffectPreview& preview : EFFECT_PREVIEWS)
    {
        mPreviewViews.push_back(findChild<LLView>(preview.widget));
    }
    for (const EffectMode& effect : EFFECT_MODES)
    {
        if (LLButton* button = findChild<LLButton>(effect.button))
        {
            const size_t index = &effect - EFFECT_MODES;
            button->setCommitCallback([this, index](LLUICtrl*, const LLSD&) { onEffectMode(index); });
            mEffectButtons.push_back({ button->getDerivedHandle<LLButton>(), -1 });
        }
        else
        {
            mEffectButtons.push_back({ LLHandle<LLButton>(), -1 });
        }
        // An amount that runs both ways, 0 off -- lens distortion, clarity --
        // catches at 0 within 1.5 % of its range, so it can be set back to 0
        // by dragging.
        LLSliderCtrl* amount = effect.amount && effect.slider ? findChild<LLSliderCtrl>(effect.slider) : nullptr;
        if (amount && amount->getMinValue() < 0.f && amount->getMaxValue() > 0.f)
        {
            const F32 snap = 0.015f * (amount->getMaxValue() - amount->getMinValue());
            const std::string setting = effect.amount;
            amount->setCommitCallback([setting, snap](LLUICtrl*, const LLSD& value)
            {
                const F32 v = (F32)value.asReal();
                if (v != 0.f && std::fabs(v) < snap)
                {
                    gSavedSettings.setF32(setting, 0.f); // the slider follows its setting
                }
            });
        }
    }
    refreshEffectSwitches();
    refreshCapture();
    if (LLButton* choose = findChild<LLButton>("snapshot_folder_choose"))
    {
        choose->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            const std::string current = gSavedPerAccountSettings.getString("SnapshotBaseDir");
            (new LLDirPickerThread([](const std::vector<std::string>& dirs, std::string)
            {
                if (!dirs.empty() && !dirs[0].empty())
                {
                    gSavedPerAccountSettings.setString("SnapshotBaseDir", dirs[0]);
                }
            }, current))->getFile();
        });
    }
    for (const char* tab : TABS)
    {
        if (LLView* stack = findChild<LLView>(std::string(tab) + "_stack"))
        {
            for (LLView* row : *stack->getChildList())
            {
                // Rows only: a layout stack also keeps a resize bar per row.
                if (dynamic_cast<LLLayoutPanel*>(row))
                {
                    mRowHeights[row] = row->getRect().getHeight();
                }
            }
        }
    }
    fitTabs();

    // The sidebar: the box with the tabs' content on the left, the column on
    // the right. The floater draws nothing itself; the box and the column do.
    mTabs = findChild<LLTabContainer>("luminous_tabs");
    // A tab's hint belongs to its button in the column, not to the panel:
    // as the panel's own it would show for the whole tab, and the tooltip
    // manager keeps a hint up while the pointer stays inside its rect, so
    // the next row's hint would wait for it to time out. Taken off the
    // panels here, once mTabs is known.
    if (LLTabContainer* tab_container = mTabs)
    {
        for (S32 i = 0; i < tab_container->getTabCount(); ++i)
        {
            if (LLPanel* panel = tab_container->getPanelByIndex(i))
            {
                mTabHints[panel->getName()] = panel->getToolTip();
                panel->setToolTip(LLStringUtil::null);
            }
        }
    }
    mBox = findChild<LLPanel>("luminous_box");
    setBackgroundVisible(false);
    mOpenWidth = getRect().getWidth();
    LLView::Params column;
    column.name("luminous_side_tabs");
    column.rect(LLRect(mOpenWidth - LuminousSideTabs::WIDTH, getRect().getHeight(), mOpenWidth, 0));
    column.follows.flags(FOLLOWS_RIGHT | FOLLOWS_TOP | FOLLOWS_BOTTOM);
    column.mouse_opaque(true);
    mSideTabs = new LuminousSideTabs(column, this);
    addChild(mSideTabs);
    if (mTabs)
    {
        mTabs->selectTabByName(gSavedSettings.getString("LuminousSidebarTab"));
    }
    setFolded(gSavedSettings.getBOOL("LuminousSidebarFolded"));
    return true;
}

void LuminousFloater::setFolded(bool folded)
{
    mFolded = folded;
    gSavedSettings.setBOOL("LuminousSidebarFolded", folded);
    if (mBox)
    {
        mBox->setVisible(!folded);
    }
    dock();
}

void LuminousFloater::onSideTab(S32 index)
{
    if (!mTabs)
    {
        return;
    }
    if (!mFolded && index == mTabs->getCurrentPanelIndex())
    {
        setFolded(true);
        return;
    }
    mTabs->selectTab(index);
    dock();
    if (LLPanel* panel = mTabs->getCurrentPanel())
    {
        gSavedSettings.setString("LuminousSidebarTab", panel->getName());
    }
    if (mFolded)
    {
        setFolded(false);
    }
}

// Against the right edge of the space floaters may use -- below the menu and
// navigation bars, clear of the toolbars. The box is only as tall as its
// tab's rows need, up to that space (beyond it the rows scroll); the column
// as tall as its items. The floater is as tall as the taller of the two and
// draws nothing itself, so the world shows round them -- though a click in
// the floater's empty corner below the shorter one does not reach the world.
void LuminousFloater::dock()
{
    if (!gFloaterView || !mSideTabs)
    {
        return;
    }
    LLRect space = gFloaterView->getSnapRect();
    // Firestorm's navigation and favourites bars sit at the top of the world,
    // inside the space floaters may use: stay below whichever is showing.
    if (mTopBars.empty() && gViewerWindow)
    {
        if (LLView* nav = gViewerWindow->getRootView()->findChildView("navigation_bar", true))
        {
            for (const char* name : { "navigation_bar_visibility_panel", "favorites_bar_visibility_panel" })
            {
                if (LLView* part = nav->findChildView(name, true))
                {
                    mTopBars.push_back(part->getHandle());
                }
            }
        }
    }
    for (const LLHandle<LLView>& handle : mTopBars)
    {
        LLView* part = handle.get();
        if (part && part->isInVisibleChain() && part->getRect().getHeight() > 0)
        {
            LLRect r;
            part->localRectToOtherView(part->getLocalRect(), &r, gFloaterView);
            space.mTop = llmin(space.mTop, r.mBottom - 2);
        }
    }
    const S32 most = space.getHeight();
    const S32 column = llmin(mSideTabs->itemsHeight(), most);
    S32 box = 0;
    if (!mFolded && mTabs && mBox)
    {
        LLPanel* panel = mTabs->getCurrentPanel();
        S32 rows = 0;
        if (panel)
        {
            if (LLView* content = panel->findChild<LLView>(panel->getName() + "_content"))
            {
                rows = content->getRect().getHeight();
            }
            if (LLTextBox* title = mBox->findChild<LLTextBox>("luminous_box_title"))
            {
                title->setText(panel->getLabel());
            }
        }
        // The box's title, the tab's rows, and its footer (reset, Render image).
        box = llmin(BOX_TITLE_HEIGHT + rows + FOOTER_HEIGHT, most);
    }
    const S32 height = llmax(column, box);
    const S32 width = mFolded ? LuminousSideTabs::WIDTH : mOpenWidth;
    const LLRect rect(space.mRight - width, space.mTop, space.mRight, space.mTop - height);
    if (rect != getRect())
    {
        setShape(rect);
    }
    if (mBox && !mFolded)
    {
        const LLRect box_rect(0, height, mBox->getRect().getWidth(), height - box);
        if (box_rect != mBox->getRect())
        {
            mBox->setShape(box_rect);
        }
    }
    reserveRight(isInVisibleChain() ? space.mRight - rect.mLeft + RESERVE_GAP : 0);
}

namespace
{
    S32 sReservedRight = 0;
    S32 sChicletReserved = 0; // what the chiclet container was narrowed by
}

// static
S32 LuminousFloater::reservedRight()
{
    return sReservedRight;
}

// Firestorm's notification icons and notice pop-ups move left of the
// sidebar: the chiclet container (top right or bottom right, as Firestorm is
// set) is narrowed from the right, and the pop-up channels placed again in
// their region, which getChannelRect narrows by the same.
void LuminousFloater::reserveRight(S32 width)
{
    width = llmax(width, 0);
    if (width == sReservedRight)
    {
        return;
    }
    sReservedRight = width;
    if (LLPanel* chiclets = gViewerWindow ? gViewerWindow->getChicletContainer() : nullptr)
    {
        const LLRect r = chiclets->getRect();
        const S32 free_right = r.mRight + sChicletReserved;
        const S32 right = llmax(free_right - width, r.mLeft + 20);
        sChicletReserved = free_right - right;
        chiclets->setShape(LLRect(r.mLeft, r.mTop, right, r.mBottom));
    }
    for (LLNotificationsUI::LLChannelManager::ChannelElem& elem : LLNotificationsUI::LLChannelManager::getInstance()->getChannelList())
    {
        if (LLNotificationsUI::LLScreenChannelBase* channel = elem.channel.get())
        {
            channel->reposition();
        }
    }
}

namespace
{
    void tell(const std::string& message)
    {
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
    }
}

namespace
{
    // The HSL sliders edit the colour range picked in the menu.
    std::string hslSetting(LLFloater* floater, const char* part)
    {
        LLComboBox* range = floater->findChild<LLComboBox>("look_hsl_range");
        return std::string("LuminousPhotoHSL") + (range ? range->getValue().asString() : std::string("Reds")) + part;
    }
    const char* const HSL_PARTS[3] = { "Hue", "Sat", "Lum" };
    const char* const HSL_SLIDERS[3] = { "look_hsl_hue", "look_hsl_sat", "look_hsl_lum" };
}

namespace
{
    // A main slider, as in Apple's Photos: one value that sets a group of
    // settings. Each setting has its value at the slider's low end, its
    // middle and its high end, and follows the slider between them. The
    // settings stay free to be fine-tuned under Options; moving the main
    // slider again sets them afresh.
    struct MasterPart
    {
        const char* setting;
        F32 low;
        F32 middle;
        F32 high;
    };
    struct Master
    {
        const char* name;    // the slider's callback parameter
        const char* setting; // the slider's own value
        F32 min;             // its range; the middle is halfway
        F32 max;
        std::vector<MasterPart> parts;
    };
    const Master MASTERS[] = {
        // Light: the middles are the shipped defaults, except GTAO's, which
        // ships at 0.29 with strength 1.1.
        { "Volumetric", "LuminousPhotoVolumetricAmount", 0.f, 1.f, {
            { "LuminousPhotoVolumetricDensity",    0.001f, 0.005f, 0.025f },
            { "LuminousPhotoVolumetricAnisotropy", 0.5f,   0.6f,   0.8f },
        } },
        { "Beams", "LuminousPhotoBeamsAmount", 0.f, 1.f, {
            { "LuminousPhotoProjectorBeamsDensity",    0.1f, 0.4f, 1.2f },
            { "LuminousPhotoProjectorBeamsAnisotropy", 0.3f, 0.3f, 0.4f },
        } },
        { "Shafts", "LuminousPhotoShaftsAmount", 0.f, 1.f, {
            { "LuminousPhotoSunShaftsStrength", 0.3f, 1.f, 2.f },
            { "LuminousPhotoSunShaftsLength",   0.8f, 0.9f, 0.95f },
        } },
        { "AO", "LuminousPhotoAOAmount", 0.f, 1.f, {
            { "LuminousPhotoAORadius",   0.25f, 0.5f, 1.f },
            { "LuminousPhotoAOStrength", 0.7f,  1.f,  1.6f },
        } },
        // Lens: from a deep focus to a shallow one.
        { "DoF", "LuminousPhotoDoFBlur", 0.f, 1.f, {
            { "CameraFNumber", 16.f, 5.6f, 1.4f },
            { "CameraMaxCoF",  5.f,  15.f, 40.f },
        } },
        // Colour.
        { "Colour", "LuminousPhotoColourAmount", -1.f, 1.f, {
            { "LuminousPhotoSaturation", 0.4f, 1.f, 1.6f },
            { "LuminousPhotoVibrance",  -0.4f, 0.f, 0.6f },
        } },
        { "SplitTone", "LuminousPhotoSplitTone", -1.f, 1.f, {
            { "LuminousPhotoShadowsWarmth",     0.6f, 0.f, -0.6f },
            { "LuminousPhotoHighlightsWarmth", -0.6f, 0.f,  0.6f },
        } },
    };

    // Each Options switch and the part of the window it shows.
    const std::pair<const char*, const char*> OPTIONS[] = {
        { "LuminousUIVolumetricOptions",   "vol_options_lp" },
        { "LuminousUIBeamsOptions",        "beams_options_lp" },
        { "LuminousUIShaftsOptions",       "shafts_options_lp" },
        { "LuminousUISoftShadowsOptions",  "softsh_options_lp" },
        { "LuminousUIContactOptions",      "contact_options_lp" },
        { "LuminousUIAOOptions",           "ao_options_lp" },
        { "LuminousUIDoFOptions",          "dof_options_lp" },
        { "LuminousUIVignetteOptions",     "vig_options_lp" },
        { "LuminousUIBloomOptions",        "bloom_options_lp" },
        { "LuminousUIObjectGlowOptions",   "object_glow_options_lp" },
        { "LuminousUIColourOptions",       "colour_options_lp" },
        { "LuminousUIWhiteBalanceOptions", "wb_options_lp" },
        { "LuminousUISplitToneOptions",    "split_options_lp" },
        { "LuminousUIMonoOptions",         "mono_options_lp" },
        { "LuminousUIMonoOptions",         "look_sepia_row_lp" },
        { "LuminousUIHSLOptions",          "look_hsl_lp" },
        { "LuminousUIHSLOptions",          "hsl_sliders_lp" },
        { "LuminousUIGrainOptions",        "grain_options_lp" },
        { "LuminousUIAnamorphicOptions",   "anamorphic_options_lp" },
        { "LuminousUILensFlareOptions",    "lens_flare_options_lp" },
        { "LuminousUIAlphaSortOptions",    "alpha_sort_options_lp" },
    };

    // The radius sliders run on a logarithmic scale: each step along one
    // changes the radius by the same share, so a small radius is set as
    // finely as a large one. The slider holds its position, 0 to 1, and the
    // text beside it the radius in metres.
    struct LogSlider
    {
        const char* slider;  // also the slider's callback parameter
        const char* text;
        const char* setting;
        F32 min;
        F32 max;
        const char* format;
        F32 offset = 0.f; // added before the logarithm, so that a scale can start at 0
    };
    const LogSlider LOG_SLIDERS[] = {
        { "gtao_radius",   "gtao_radius_value",   "LuminousPhotoAORadius",     0.1f, 2.f, "%.2f" },
        // contact shadows' length: most of the slider below 15 cm, where it
        // is set
        { "contact_length", "contact_length_value", "LuminousPhotoContactShadowsLength", 0.01f, 1.f, "%.2f" },
        // soft shadows' Softness, 0 (sharp) to 10 suns, with 1 -- the real
        // sun -- at the middle: under trees the whole change from sharp leaf
        // shadows to none lies between 0 and 1
        { "softsh_amount", "softsh_amount_value", "LuminousPhotoShadowSoftness", 0.f, 10.f, "%.2f", 0.125f },
    };

    // Sliders on one part of a Firestorm vector setting: Shadow clarity is
    // the y of RenderShadowSplitExponent.
    struct VectorSlider
    {
        const char* slider; // also the slider's callback parameter
        const char* setting;
        S32 component;
    };
    const VectorSlider VECTOR_SLIDERS[] = {
        { "shadow_clarity", "RenderShadowSplitExponent", VY },
    };

    F32 logPosition(const LogSlider& s, F32 value)
    {
        const F32 lo = s.min + s.offset;
        const F32 hi = s.max + s.offset;
        return logf((llclamp(value, s.min, s.max) + s.offset) / lo) / logf(hi / lo);
    }

    // Rounded to the hundredth, as the text shows it.
    F32 logValue(const LogSlider& s, F32 position)
    {
        const F32 lo = s.min + s.offset;
        const F32 hi = s.max + s.offset;
        return llmax(ll_round(lo * powf(hi / lo, llclamp(position, 0.f, 1.f)) - s.offset, 0.01f), s.min);
    }
}

void LuminousFloater::onMaster(const LLSD& which)
{
    for (const Master& master : MASTERS)
    {
        if (which.asString() != master.name)
        {
            continue;
        }
        const F32 mid = 0.5f * (master.min + master.max);
        const F32 v = llclamp(gSavedSettings.getF32(master.setting), master.min, master.max);
        for (const MasterPart& part : master.parts)
        {
            const F32 value = v >= mid ? lerp(part.middle, part.high, (v - mid) / (master.max - mid))
                                       : lerp(part.middle, part.low, (mid - v) / (mid - master.min));
            gSavedSettings.setF32(part.setting, value);
        }
    }
}

void LuminousFloater::onLogSlider(const LLSD& which)
{
    for (const LogSlider& s : LOG_SLIDERS)
    {
        LLSliderCtrl* slider = findChild<LLSliderCtrl>(s.slider);
        if (slider && which.asString() == s.slider)
        {
            gSavedSettings.setF32(s.setting, logValue(s, slider->getValueF32()));
        }
    }
    refreshLogSliders();
}

void LuminousFloater::onVectorSlider(const LLSD& which)
{
    for (const VectorSlider& v : VECTOR_SLIDERS)
    {
        LLSliderCtrl* slider = findChild<LLSliderCtrl>(v.slider);
        if (slider && which.asString() == v.slider)
        {
            LLVector3 value = gSavedSettings.getVector3(v.setting);
            value.mV[v.component] = slider->getValueF32();
            gSavedSettings.setVector3(v.setting, value);
        }
    }
}

void LuminousFloater::refreshVectorSliders()
{
    for (const VectorSlider& v : VECTOR_SLIDERS)
    {
        LLSliderCtrl* slider = findChild<LLSliderCtrl>(v.slider);
        const F32 value = gSavedSettings.getVector3(v.setting).mV[v.component];
        if (slider && fabsf(slider->getValueF32() - value) > 1e-4f)
        {
            slider->setValue(value);
        }
    }
}

// To the settings, which a main slider, a reset or a preset may have
// changed. A slider already at its setting is left alone, so one being
// dragged does not jump back to the centimetre.
void LuminousFloater::refreshLogSliders()
{
    for (const LogSlider& s : LOG_SLIDERS)
    {
        LLSliderCtrl* slider = findChild<LLSliderCtrl>(s.slider);
        if (!slider)
        {
            continue;
        }
        const F32 value = llclamp(gSavedSettings.getF32(s.setting), s.min, s.max);
        if (fabsf(logValue(s, slider->getValueF32()) - value) > 0.005f)
        {
            slider->setValue(logPosition(s, value));
        }
        if (LLTextBox* text = findChild<LLTextBox>(s.text))
        {
            text->setText(llformat(s.format, value));
        }
    }
}

void LuminousFloater::refreshOptions()
{
    for (const auto& option : OPTIONS)
    {
        LLLayoutPanel* panel = findChild<LLLayoutPanel>(option.second);
        const bool shown = gSavedSettings.getBOOL(option.first);
        if (panel && panel->getVisible() != shown)
        {
            panel->setVisible(shown);
            fitTabs();
        }
    }
}

// Each tab's rows sit in a panel that scrolls when they are taller than the
// window; it is as tall as the rows shown, so it scrolls just as far as it must.
void LuminousFloater::fitTabs()
{
    for (const char* tab : TABS)
    {
        LLView* stack = findChild<LLView>(std::string(tab) + "_stack");
        LLView* content = findChild<LLView>(std::string(tab) + "_content");
        if (!stack || !content)
        {
            continue;
        }
        S32 height = 0;
        for (LLView* row : *stack->getChildList())
        {
            auto known = mRowHeights.find(row);
            if (row->getVisible() && known != mRowHeights.end())
            {
                height += known->second;
            }
        }
        if (height > 0 && content->getRect().getHeight() != height)
        {
            // Kept at its top, so the rows above an opened or closed effect
            // stay where they are.
            const LLRect old = content->getRect();
            content->setShape(LLRect(old.mLeft, old.mTop, old.mRight, old.mTop - height));
            stack->setShape(LLRect(0, height, old.getWidth(), 0));
        }
    }
}

void LuminousFloater::refreshHSL()
{
    for (S32 k = 0; k < 3; ++k)
    {
        LLUICtrl* slider = findChild<LLUICtrl>(HSL_SLIDERS[k]);
        LLControlVariable* control = gSavedSettings.getControl(hslSetting(this, HSL_PARTS[k]));
        if (slider && control && slider->getValue().asReal() != control->getValue().asReal())
        {
            slider->setValue(control->getValue());
        }
    }
}

void LuminousFloater::onHSLChanged()
{
    for (S32 k = 0; k < 3; ++k)
    {
        LLUICtrl* slider = findChild<LLUICtrl>(HSL_SLIDERS[k]);
        LLControlVariable* control = gSavedSettings.getControl(hslSetting(this, HSL_PARTS[k]));
        if (slider && control)
        {
            control->setValue(slider->getValue());
        }
    }
}

void LuminousFloater::refreshLUTs()
{
    LLComboBox* luts = findChild<LLComboBox>("look_lut");
    if (!luts)
    {
        return;
    }
    luts->removeall();
    luts->add("None", LLSD(""));
    for (const std::string& name : LuminousPhotoMode::lutNames())
    {
        luts->add(name, LLSD(name));
    }
    luts->setValue(LLSD(gSavedSettings.getString("LuminousPhotoLUT")));
}

void LuminousFloater::onLoadLUT()
{
    LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (files.empty())
            {
                return;
            }
            std::string name;
            std::string error;
            if (!LuminousPhotoMode::importLUT(files[0], name, error))
            {
                tell("Could not use that table: " + error + ".");
                return;
            }
            gSavedSettings.setString("LuminousPhotoLUT", name);
            if (LuminousFloater* floater = dynamic_cast<LuminousFloater*>(handle.get()))
            {
                floater->refreshLUTs();
            }
            tell("Added the table " + name + ".");
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void LuminousFloater::refreshTonemapper()
{
    // The Look follows Phototools' tonemapper (LuminousPhotoTonemapper -1)
    // until one is picked here; show which one that is, without picking it.
    static LLCachedControl<S32> look_tonemapper(gSavedSettings, "LuminousPhotoTonemapper", -1);
    static LLCachedControl<U32> firestorm_tonemapper(gSavedSettings, "RenderTonemapType", 0);
    LLComboBox* combo = findChild<LLComboBox>("look_tonemapper");
    if (combo && look_tonemapper() < 0 && combo->getValue().asInteger() != (S32)firestorm_tonemapper())
    {
        combo->setValue(LLSD((S32)llmin((U32)firestorm_tonemapper(), 1U)));
    }
    // Likewise the strength: the sky's tone mix until one is set here.
    static LLCachedControl<F32> look_mix(gSavedSettings, "LuminousPhotoTonemapMix", -1.f);
    static LLCachedControl<bool> auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    LLUICtrl* slider = findChild<LLUICtrl>("look_tonemap_mix");
    // No environment before login: getCurrentSky would read through a null one.
    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t sky = environment.getCurrentEnvironmentInstance() ? environment.getCurrentSky() : LLSettingsSky::ptr_t();
    if (slider && look_mix() < 0.f && sky)
    {
        slider->setValue(LLSD(sky->getTonemapMix(auto_adjust())));
    }
    // Under a legacy (Windlight) sky Firestorm draws without tonemapping (its NO_POST
    // programs: the sky's probe ambiance is 0, LLPipeline::tonemap), so the
    // tonemapper and its strength do nothing there. Greyed, with a line
    // saying so above Firestorm's Auto-adjust legacy skies, which makes the
    // sky a modern one.
    const bool untonemapped = sky && sky->getReflectionProbeAmbiance(auto_adjust()) == 0.f;
    if (combo)
    {
        combo->setEnabled(!untonemapped);
    }
    if (slider)
    {
        slider->setEnabled(!untonemapped);
    }
    LLView* note = findChild<LLView>("look_legacy_note_lp");
    if (note && note->getVisible() != untonemapped)
    {
        note->setVisible(untonemapped);
        fitTabs();
    }
}

void LuminousFloater::refreshShots(const std::string& select)
{
    LLComboBox* shots = findChild<LLComboBox>("shot_presets");
    if (!shots)
    {
        return;
    }
    shots->removeall();
    for (const std::string& name : LuminousPhotoMode::shotNames())
    {
        shots->add(name, LLSD(name));
    }
    if (!select.empty())
    {
        shots->setSimple(select);
    }
    else
    {
        shots->clear();
    }
}

namespace
{
    std::string shotName(LLFloater* floater)
    {
        LLComboBox* shots = floater->findChild<LLComboBox>("shot_presets");
        std::string name = shots ? shots->getSimple() : std::string();
        LLStringUtil::trim(name);
        return name;
    }
}

void LuminousFloater::onSaveShot()
{
    const std::string name = shotName(this);
    if (name.empty())
    {
        tell("Type a name for the shot in the box, then Save.");
        return;
    }
    if (!LuminousPhotoMode::saveShot(name))
    {
        tell("Could not save the shot " + name + ".");
        return;
    }
    refreshShots(name);
    tell("Saved the shot " + name + ": camera, field of view and depth of field.");
}

void LuminousFloater::onGoToShot()
{
    const std::string name = shotName(this);
    std::string error;
    if (!name.empty() && !LuminousPhotoMode::goToShot(name, error))
    {
        tell("Could not go to the shot " + name + ": " + error + ".");
    }
}

void LuminousFloater::onDeleteShot()
{
    const std::string name = shotName(this);
    if (!name.empty() && LuminousPhotoMode::deleteShot(name))
    {
        refreshShots();
        tell("Deleted the shot " + name + ".");
    }
}

void LuminousFloater::refreshPresets(const std::string& select)
{
    LLComboBox* presets = findChild<LLComboBox>("preset_names");
    if (!presets)
    {
        return;
    }
    presets->removeall();
    for (const std::string& name : LuminousPhotoMode::presetNames())
    {
        presets->add(name, LLSD(name));
    }
    if (!select.empty())
    {
        presets->setSimple(select);
    }
    else
    {
        presets->clear();
    }
}

namespace
{
    std::string presetName(LLFloater* floater)
    {
        LLComboBox* presets = floater->findChild<LLComboBox>("preset_names");
        std::string name = presets ? presets->getSimple() : std::string();
        LLStringUtil::trim(name);
        return name;
    }
}

void LuminousFloater::onSavePreset()
{
    const std::string name = presetName(this);
    if (name.empty())
    {
        tell("Type a name for the preset in the box, then Save.");
        return;
    }
    if (!LuminousPhotoMode::savePreset(name))
    {
        tell("Could not save the preset " + name + ".");
        return;
    }
    refreshPresets(name);
    tell("Saved the preset " + name + ": the Scene, Tone, Colour and Effects tabs.");
}

void LuminousFloater::onLoadPreset()
{
    const std::string name = presetName(this);
    if (name.empty())
    {
        tell("Pick a preset in the box, then Load.");
        return;
    }
    if (!LuminousPhotoMode::applyPreset(name))
    {
        tell("Could not load the preset " + name + ".");
        return;
    }
    tell("Loaded the preset " + name + ".");
}

void LuminousFloater::onDeletePreset()
{
    const std::string name = presetName(this);
    if (!name.empty() && LuminousPhotoMode::deletePreset(name))
    {
        refreshPresets();
        tell("Deleted the preset " + name + ".");
    }
}

std::string LuminousFloater::tabHint(LLPanel* panel) const
{
    const auto hint = panel ? mTabHints.find(panel->getName()) : mTabHints.end();
    return hint != mTabHints.end() ? hint->second : std::string();
}

void LuminousFloater::scanSkies()
{
    LLHandle<LLFloater> handle = getHandle();
    LuminousPhotoMode::scanInventorySkies([handle]()
    {
        if (LuminousFloater* floater = static_cast<LuminousFloater*>(handle.get()))
        {
            // not under the pointer: an open list is refilled when next opened
            LLComboBox* skies = floater->findChild<LLComboBox>("env_sky");
            LLView* list = skies ? skies->findChild<LLView>("ComboBox") : nullptr;
            if (!list || !list->getVisible())
            {
                floater->refreshSkies();
            }
        }
    });
}

void LuminousFloater::refreshSkies()
{
    LLComboBox* skies = findChild<LLComboBox>("env_sky");
    if (!skies)
    {
        return;
    }
    skies->removeall();
    skies->add("Region's sky", LLSD(std::string()));
    for (const std::string& name : LuminousPhotoMode::environmentNames())
    {
        skies->add(name, LLSD(name));
    }
    const auto own = LuminousPhotoMode::inventoryPBRSkies();
    if (!own.empty())
    {
        skies->addSeparator();
        for (const auto& [name, value] : own)
        {
            skies->add(name, LLSD(value));
        }
    }
    skies->setValue(gSavedSettings.getString("LuminousEnvTemplate"));
}

// static
void LuminousFloater::openAtLogin()
{
    if (LuminousFloater* sidebar = LLFloaterReg::showTypedInstance<LuminousFloater>("luminous"))
    {
        sidebar->setFolded(true); // just its column of tabs
    }
}

void LuminousFloater::onOpen(const LLSD& key)
{
    // Opened with a tab's key -- "light" (Lighting), "look" (Tone),
    // "colour", "lens" (Effects), "snapshot" (Render), "presets" (Save / Load), "graphics" (Settings) -- it shows that
    // tab: Floater.Show luminous lens, or LLFloaterReg::showInstance.
    if (key.isString() && !key.asString().empty())
    {
        onShowTab(key);
    }
    dock();

    // Not at the login screen: opened there (a script, LEAP), it stays
    // hidden and shows once the viewer is in the world.
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        setVisible(false);
        LLHandle<LLFloater> handle = getHandle();
        doOnIdleRepeating([handle]()
        {
            LLFloater* floater = handle.get();
            if (!floater)
            {
                return true;
            }
            if (LLStartUp::getStartupState() < STATE_STARTED)
            {
                return false; // call again
            }
            floater->setVisible(true);
            return true;
        });
    }
}

void LuminousFloater::refreshPauseButtons()
{
    // The pause button by Render image on every tab shows what a click
    // does, as a player's: pause, or, the scene paused, play again
    static LLCachedControl<bool> paused(gSavedSettings, "LuminousScenePaused", false);
    const S32 shown = paused ? 1 : 0;
    if (shown == mPauseShown)
    {
        return;
    }
    mPauseShown = shown;
    for (const char* tab : { "light_defaults", "look_reset", "colour_defaults", "lens_defaults", "snapshot_defaults", "presets", "graphics_reset" })
    {
        if (LLButton* button = findChild<LLButton>(std::string("pause_scene_") + tab))
        {
            button->setImageOverlay(paused ? "Play_Off" : "Pause_Off");
            button->setToolTip(paused ? std::string("The scene is paused. Click to go on.")
                                      : std::string("Pause everything that moves (animations, water, clouds, particles) while you move the camera freely. Click again to go on."));
        }
    }
}

void LuminousFloater::draw()
{
    // Rows first, then the box to fit them, so both change in the same frame.
    refreshOptions(); // cheap: only a change re-lays the window out
    refreshLogSliders(); // cheap: four sliders, set only on a change
    refreshVectorSliders(); // and one
    dock(); // cheap: only a change of the window, the toolbars or the rows moves it
    refreshEffectSwitches();
    refreshCapture();
    refreshPauseButtons();
    // Settings can change behind the window's back -- in preferences, by a
    // preset -- so look again twice a second while it is open.
    if (mRefreshTimer.getElapsedTimeF32() > 0.5f)
    {
        mRefreshTimer.reset();
        refreshTonemapper();
        LuminousPhotoMode::followShownSun(); // the sun's sliders show the region's sun
        refreshHSL(); // a reset or a preset may have changed them
        // A table added another way (Luminous.ImportLUT) joins the menu.
        if (LLComboBox* luts = findChild<LLComboBox>("look_lut"))
        {
            if ((size_t)luts->getItemCount() != LuminousPhotoMode::lutNames().size() + 1)
            {
                refreshLUTs();
            }
        }
        if (LLTextBox* size = findChild<LLTextBox>("snapshot_size_px"))
        {
            // What the snapshot size comes to, in pixels: the window's, or
            // twice it (luminoushdrsnapshot.cpp, capped at the largest texture).
            static LLCachedControl<U32> scale(gSavedSettings, "LuminousSnapshotScale", 1);
            const U32 most = (U32)gGLManager.mGLMaxTextureSize;
            const U32 s = llclamp((U32)scale(), 1U, 2U);
            size->setText(llformat("%u x %u", llmin((U32)gViewerWindow->getWindowWidthRaw() * s, most),
                                   llmin((U32)gViewerWindow->getWindowHeightRaw() * s, most)));
        }
        if (LLTextBox* live = findChild<LLTextBox>("live_view_size"))
        {
            // What the live view renders, before it is scaled up.
            const F32 scale = LuminousPhotoMode::chosenLiveScale();
            live->setText(llformat("%u x %u", (U32)(gViewerWindow->getWorldViewWidthRaw() * scale),
                                   (U32)(gViewerWindow->getWorldViewHeightRaw() * scale)));
        }
    }
    LLFloater::draw();
}

bool LuminousFloater::handleToolTip(S32 x, S32 y, MASK mask)
{
    for (size_t i = 0; i < mPreviewViews.size(); ++i)
    {
        const EffectPreview& preview = EFFECT_PREVIEWS[i];
        LLView* view = mPreviewViews[i];
        if (!view || !view->isInVisibleChain())
        {
            continue;
        }
        S32 local_x = 0;
        S32 local_y = 0;
        localPointToOtherView(x, y, &local_x, &local_y, view);
        if (!view->pointInView(local_x, local_y) || (preview.label_width > 0 && local_x >= preview.label_width))
        {
            continue;
        }
        LLRect local = view->getLocalRect();
        if (preview.label_width > 0)
        {
            local.mRight = preview.label_width;
        }
        LLRect screen;
        view->localRectToScreen(local, &screen);
        std::string text = preview.summary;
        for (const char* point : preview.points)
        {
            text += std::string("\n\u2022  ") + point;
        }
        LLTextBox* name = dynamic_cast<LLTextBox*>(view);
        const std::string title = preview.title ? preview.title : name ? name->getText() : std::string();
        LLToolTipMgr::instance().show(LLToolTip::Params()
            .message(text)
            .sticky_rect(screen)
            .create_params(LLSD().with("image", preview.image).with("title", title))
            .create_callback([](LLToolTip::Params p) -> LLToolTip*
                             { return LLUICtrlFactory::create<LuminousPreviewToolTip>(p); }));
        return true;
    }
    return LLFloater::handleToolTip(x, y, mask);
}

void LuminousFloater::onShowTab(const LLSD& tab)
{
    if (mTabs && mTabs->selectTabByName("luminous_" + tab.asString()))
    {
        gSavedSettings.setString("LuminousSidebarTab", "luminous_" + tab.asString());
        if (mFolded)
        {
            setFolded(false);
        }
    }
}

void LuminousFloater::onEffectMode(size_t index)
{
    const EffectMode& effect = EFFECT_MODES[index];
    const EffectState state = effectState(effect);
    const bool turn_on = state == EffectState::Off;
    const bool turn_off = state == EffectState::Render || (state == EffectState::Live && !effect.render_only);
    if (turn_on || turn_off)
    {
        if (effect.on)
        {
            gSavedSettings.setBOOL(effect.on, turn_on);
        }
        else if (turn_on)
        {
            auto kept = sEffectKept.find(effect.amount);
            gSavedSettings.setF32(effect.amount, kept != sEffectKept.end() ? kept->second : effect.start);
        }
        else
        {
            sEffectKept[effect.amount] = gSavedSettings.getF32(effect.amount);
            gSavedSettings.setF32(effect.amount, 0.f);
        }
        // the Firestorm switch it builds on follows it (kept out of the
        // chain above: an effect with a switch has no amount)
        if (effect.needs)
        {
            gSavedSettings.setBOOL(effect.needs, turn_on);
        }
    }
    if (effect.render_only)
    {
        // live after off; render only after live; and back to live the next time it goes on
        gSavedSettings.setBOOL(effect.render_only, state == EffectState::Live);
    }
    refreshEffectSwitches();
}

void LuminousFloater::refreshEffectSwitches()
{
    static LLUIColor off_colour = LLUIColorTable::instance().getColor("LabelDisabledColor");
    static LLUIColor live_colour = LLUIColorTable::instance().getColor("ButtonLabelColor");
    for (size_t i = 0; i < mEffectButtons.size() && i < LL_ARRAY_SIZE(EFFECT_MODES); ++i)
    {
        const EffectMode& effect = EFFECT_MODES[i];
        LLButton* button = mEffectButtons[i].first.get();
        if (!button)
        {
            continue;
        }
        const EffectState state = effectState(effect);
        if ((S32)state != mEffectButtons[i].second)
        {
            mEffectButtons[i].second = (S32)state;
            switch (state)
            {
                case EffectState::Off:
                    button->setImageOverlay("Luminous_Effect_Off", LLFontGL::HCENTER, off_colour.get());
                    button->setToolTip(effect.always_render
                        ? std::string("Off. Click to turn it on in the rendered picture.")
                        : std::string("Off. Click to turn it on."));
                    break;
                case EffectState::Live:
                    button->setImageOverlay("Luminous_Effect_Live", LLFontGL::HCENTER, live_colour.get());
                    button->setToolTip(effect.render_only
                        ? std::string("On, also while you look round. Click to show it only in the rendered picture, which keeps the view quick.")
                        : std::string("On. Click to turn it off."));
                    break;
                case EffectState::Render:
                    button->setImageOverlay("Luminous_Effect_Render", LLFontGL::HCENTER, live_colour.get()); // white, as live
                    button->setToolTip(std::string("Only in the rendered picture. Click to turn it off."));
                    break;
            }
        }
        if (effect.slider)
        {
            if (LLUICtrl* slider = findChild<LLUICtrl>(effect.slider))
            {
                slider->setEnabled(state != EffectState::Off);
            }
        }
    }
}

void LuminousFloater::refreshCapture()
{
    // Where renders go: the folder's name, the whole path in its hint.
    if (LLUICtrl* folder = findChild<LLUICtrl>("snapshot_folder"))
    {
        const std::string dir = gSavedPerAccountSettings.getString("SnapshotBaseDir");
        const std::string shown = dir.empty() ? std::string("Asked the first time") : gDirUtilp->getBaseFileName(dir);
        if (folder->getValue().asString() != shown)
        {
            folder->setValue(shown);
            folder->setToolTip(dir.empty() ? std::string("The first render asks where to save, and the folder is remembered.") : dir);
        }
    }
    static LLCachedControl<U32> format(gSavedSettings, "LuminousSnapshotFormat", 0);
    static LLCachedControl<U32> frames(gSavedSettings, "LuminousSnapshotFrames", 16);
    const U32 f = llmin((U32)format(), 4U);
    if (LLUICtrl* menu = findChild<LLUICtrl>("snapshot_format"))
    {
        // What the format is, in the menu's tooltip (the sidebar has no room
        // for a line of its own).
        menu->setToolTip(std::string(FORMAT_TEXT[f]));
    }
    // Smooth edges renders twice the size for an EXR or TIFF of one frame;
    // more frames smooth the edges anyway, and a PNG is read after
    // anti-aliasing.
    if (LLUICtrl* smooth = findChild<LLUICtrl>("snapshot_smooth"))
    {
        smooth->setEnabled(frames() <= 1 && (f == 1 || f == 2));
    }
}

// <Luminous> The render window.

LuminousRenderFloater::LuminousRenderFloater(const LLSD& key)
    : LLFloater(key)
{
}

LuminousRenderFloater::~LuminousRenderFloater() = default;

bool LuminousRenderFloater::postBuild()
{
    getChild<LLComboBox>("destination")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    getChild<LLComboBox>("shape")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    getChild<LLComboBox>("tex_shape")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    getChild<LLComboBox>("tex_size")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refresh(); });
    getChild<LLComboBox>("shape")->selectByValue(0);
    getChild<LLComboBox>("tex_shape")->selectByValue(0);
    getChild<LLComboBox>("tex_size")->selectByValue(2048);
    getChild<LLComboBox>("flickr_rating")->selectByValue(1);
    getChild<LLComboBox>("primfeed_rating")->selectByValue(1);
    getChild<LLButton>("send")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    // Sign-ins started here, as Firestorm's windows start them when they
    // open, so that none of those windows opens beside this one. Flickr:
    // the browser, then the code it gives back into a prompt; Primfeed: the
    // browser, then a message in world.
    getChild<LLButton>("flickr_connect")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        LLFlickrConnect::instance().setConnectionState(LLFlickrConnect::FLICKR_CONNECTION_IN_PROGRESS);
        new exoFlickrAuth([](bool success, const LLSD&)
        {
            LLFlickrConnect::instance().setConnectionState(success ? LLFlickrConnect::FLICKR_CONNECTED
                                                                   : LLFlickrConnect::FLICKR_CONNECTION_FAILED);
        });
    });
    getChild<LLButton>("primfeed_connect")->setCommitCallback([](LLUICtrl*, const LLSD&)
        { FSPrimfeedAuth::initiateAuthRequest(); });
    std::string name;
    LLAgentUI::buildFullname(name);
    getChild<LLLineEditor>("email_from")->setText(name);
    return true;
}

void LuminousRenderFloater::showPicture(const LLPointer<LLImageRaw>& picture, U32 destination)
{
    if (LuminousRenderFloater* floater = LLFloaterReg::showTypedInstance<LuminousRenderFloater>("luminous_render"))
    {
        floater->setPicture(picture, destination);
    }
}

void LuminousRenderFloater::onClose(bool app_quitting)
{
    LLFloater::onClose(app_quitting);
    // back to the render it came from, with the keyboard (Esc closes it)
    if (!app_quitting)
    {
        if (LLFloater* overlay = LLFloaterReg::findInstance("luminous_picture"))
        {
            if (overlay->getVisible())
            {
                overlay->setFocus(true);
            }
        }
    }
}

void LuminousRenderFloater::setPicture(const LLPointer<LLImageRaw>& picture, U32 destination)
{
    mPicture = picture;
    mPreview = nullptr;
    if (mPicture.notNull() && !mPicture->isBufferInvalid())
    {
        // A copy small enough to show: the window is not 10,000 pixels wide.
        LLPointer<LLImageRaw> small = new LLImageRaw(mPicture->getData(), mPicture->getWidth(), mPicture->getHeight(),
                                                     mPicture->getComponents());
        const F32 shrink = llmin(1.f, 1600.f / (F32)llmax(small->getWidth(), small->getHeight()));
        if (shrink < 1.f)
        {
            small->scale(llmax(1, ll_round(small->getWidth() * shrink)), llmax(1, ll_round(small->getHeight() * shrink)));
        }
        // A texture's sides must be powers of two; drawn back at the
        // picture's own shape.
        small->biasedScaleToPowerOfTwo(2048);
        mPreview = LLViewerTextureManager::getLocalTexture(small.get(), false);
        getChild<LLUICtrl>("picture_size")->setValue(llformat("The picture: %d x %d", mPicture->getWidth(), mPicture->getHeight()));
        std::string location;
        LLAgentUI::buildLocationString(location, LLAgentUI::LOCATION_FORMAT_NORMAL);
        getChild<LLLineEditor>("tex_name")->setText("Render : " + location);
        std::string who;
        LLAgentUI::buildFullname(who);
        getChild<LLLineEditor>("tex_desc")->setText("Taken by " + who + " with Luminous at " + location);
    }
    if (destination >= 1 && destination <= 5)
    {
        getChild<LLComboBox>("destination")->selectByValue((S32)destination);
    }
    getChild<LLUICtrl>("status")->setValue(LLSD());
    getChild<LLButton>("send")->setEnabled(mPicture.notNull());
    refresh();
}

U32 LuminousRenderFloater::destination() const
{
    return (U32)llclamp(getChild<LLComboBox>("destination")->getValue().asInteger(), 1, 5);
}

void LuminousRenderFloater::textureShape(S32& tex_w, S32& tex_h, LLRect& part) const
{
    const S32 longest = llclamp(getChild<LLComboBox>("tex_size")->getValue().asInteger(), 64, 2048);
    const S32 w = mPicture.notNull() ? mPicture->getWidth() : 16;
    const S32 h = mPicture.notNull() ? mPicture->getHeight() : 9;
    const F32 aspect = (F32)w / (F32)llmax(h, 1);
    // The power-of-two shape nearest the picture's: the longer side as
    // chosen, the shorter the power of two nearest in proportion.
    auto nearest_pow2 = [](F32 v)
    {
        S32 p = 1;
        while (p * 2 <= 2048 && (F32)(p * 2) <= v * 1.41421356f)
        {
            p *= 2;
        }
        return llmax(p, 16);
    };
    // The texture's shape: the nearest to the picture's, or square, wide or
    // tall as chosen; the picture is fitted to it below.
    switch (getChild<LLComboBox>("tex_shape")->getValue().asInteger())
    {
        case 1: // square
            tex_w = tex_h = longest;
            break;
        case 2: // wide, 2:1
            tex_w = longest;
            tex_h = longest / 2;
            break;
        case 3: // tall, 1:2
            tex_w = longest / 2;
            tex_h = longest;
            break;
        default:
            if (aspect >= 1.f)
            {
                tex_w = longest;
                tex_h = llmin(longest, nearest_pow2((F32)longest / aspect));
            }
            else
            {
                tex_h = longest;
                tex_w = llmin(longest, nearest_pow2((F32)longest * aspect));
            }
            break;
    }
    const S32 shape = getChild<LLComboBox>("shape")->getValue().asInteger();
    const F32 tex_aspect = (F32)tex_w / (F32)tex_h;
    if (shape == 0)
    {
        // cut: the largest part of the picture with the texture's shape
        S32 cw = w, ch = h;
        if (aspect > tex_aspect)
        {
            cw = llmax(1, ll_round((F32)h * tex_aspect));
        }
        else
        {
            ch = llmax(1, ll_round((F32)w / tex_aspect));
        }
        part.setLeftTopAndSize((w - cw) / 2, (h + ch) / 2, cw, ch);
    }
    else
    {
        part.setLeftTopAndSize(0, h, w, h);
    }
}

LLPointer<LLImageRaw> LuminousRenderFloater::makeTexture() const
{
    S32 tex_w, tex_h;
    LLRect part;
    textureShape(tex_w, tex_h, part);
    const S32 shape = getChild<LLComboBox>("shape")->getValue().asInteger();
    const S32 c = mPicture->getComponents();
    // the part of the picture used
    LLPointer<LLImageRaw> cut = new LLImageRaw(part.getWidth(), part.getHeight(), c);
    const U8* src = mPicture->getData();
    U8* dst = cut->getData();
    for (S32 y = 0; y < part.getHeight(); ++y)
    {
        memcpy(dst + (size_t)y * part.getWidth() * c,
               src + ((size_t)(part.mBottom + y) * mPicture->getWidth() + part.mLeft) * c, (size_t)part.getWidth() * c);
    }
    if (shape != 1)
    {
        cut->scale(tex_w, tex_h); // cut to the shape, or stretched to it
        return cut;
    }
    // the whole picture inside the texture, black bars round it
    const F32 fit = llmin((F32)tex_w / part.getWidth(), (F32)tex_h / part.getHeight());
    const S32 fw = llclamp(ll_round(part.getWidth() * fit), 1, tex_w);
    const S32 fh = llclamp(ll_round(part.getHeight() * fit), 1, tex_h);
    cut->scale(fw, fh);
    LLPointer<LLImageRaw> canvas = new LLImageRaw(tex_w, tex_h, c);
    canvas->fill(LLColor4U(0, 0, 0, 255));
    const S32 x0 = (tex_w - fw) / 2;
    const S32 y0 = (tex_h - fh) / 2;
    for (S32 y = 0; y < fh; ++y)
    {
        memcpy(canvas->getData() + ((size_t)(y0 + y) * tex_w + x0) * c, cut->getData() + (size_t)y * fw * c, (size_t)fw * c);
    }
    return canvas;
}

LLPointer<LLImageRaw> LuminousRenderFloater::scaledToFit(S32 longest) const
{
    LLPointer<LLImageRaw> copy = new LLImageRaw(mPicture->getData(), mPicture->getWidth(), mPicture->getHeight(),
                                                mPicture->getComponents());
    const F32 shrink = llmin(1.f, (F32)longest / (F32)llmax(copy->getWidth(), copy->getHeight()));
    if (shrink < 1.f)
    {
        copy->scale(llmax(1, ll_round(copy->getWidth() * shrink)), llmax(1, ll_round(copy->getHeight() * shrink)));
    }
    return copy;
}

void LuminousRenderFloater::refresh()
{
    const U32 where = destination();
    getChild<LLView>("inventory_panel")->setVisible(where == 1);
    getChild<LLView>("profile_panel")->setVisible(where == 2);
    getChild<LLView>("flickr_panel")->setVisible(where == 3);
    getChild<LLView>("primfeed_panel")->setVisible(where == 4);
    getChild<LLView>("email_panel")->setVisible(where == 5);
    if (where == 1 && mPicture.notNull())
    {
        S32 tex_w, tex_h;
        LLRect part;
        textureShape(tex_w, tex_h, part);
        const S32 cost = LLAgentBenefitsMgr::current().getTextureUploadCost(tex_w, tex_h);
        getChild<LLUICtrl>("tex_result")->setValue(llformat("A texture of %d x %d. Uploading costs L$%d.", tex_w, tex_h, cost));
    }
    const LLFlickrConnect::EConnectionState flickr = LLFlickrConnect::instance().getConnectionState();
    const bool flickr_ready = flickr == LLFlickrConnect::FLICKR_CONNECTED || flickr == LLFlickrConnect::FLICKR_POSTED
                              || flickr == LLFlickrConnect::FLICKR_POST_FAILED;
    getChild<LLView>("flickr_connect")->setVisible(!flickr_ready);
    getChild<LLUICtrl>("flickr_connect")->setEnabled(flickr != LLFlickrConnect::FLICKR_CONNECTION_IN_PROGRESS);
    const bool primfeed_ready = FSPrimfeedAuth::isAuthorized();
    getChild<LLView>("primfeed_connect")->setVisible(!primfeed_ready);
    getChild<LLUICtrl>("primfeed_connect")->setEnabled(!FSPrimfeedAuth::isPendingAuth());
    bool can_send = mPicture.notNull();
    if (where == 3)
    {
        can_send = can_send && flickr_ready;
    }
    else if (where == 4)
    {
        can_send = can_send && primfeed_ready;
    }
    getChild<LLButton>("send")->setEnabled(can_send);
    fitToPanel();
}

void LuminousRenderFloater::fitToPanel()
{
    // As tall as the rows of the place shown, Send just below them, so no
    // empty space is left at the bottom: each place's panel as tall as its
    // visible rows, the window after it.
    static const char* const PANELS[] = { "inventory_panel", "profile_panel", "flickr_panel", "primfeed_panel", "email_panel" };
    LLView* panel = getChild<LLView>(PANELS[destination() - 1]);
    S32 lowest = panel->getRect().getHeight();
    for (LLView* child : *panel->getChildList())
    {
        if (child->getVisible())
        {
            lowest = llmin(lowest, child->getRect().mBottom);
        }
    }
    const S32 rows = llmax(panel->getRect().getHeight() - lowest, 0);
    const LLRect p = panel->getRect();
    if (rows > 0 && rows != p.getHeight())
    {
        panel->setShape(LLRect(p.mLeft, p.mTop, p.mRight, p.mTop - rows));
    }
    // the panel's top from the window's, then a gap, Send, and the margin
    // under it, as laid out in the file
    const LLView* send = getChild<LLView>("send");
    const S32 height = (getRect().getHeight() - p.mTop) + rows + 12 + send->getRect().getHeight() + send->getRect().mBottom;
    if (height != getRect().getHeight())
    {
        const LLRect r = getRect();
        setShape(LLRect(r.mLeft, r.mTop, r.mRight, r.mTop - height));
    }
}

void LuminousRenderFloater::draw()
{
    if (mRefreshTimer.getElapsedTimeF32() > 1.f)
    {
        mRefreshTimer.reset();
        refresh(); // a sign-in in Flickr's or Primfeed's window shows here
    }
    LLFloater::draw();
    LLView* area = getChild<LLView>("picture_area");
    if (mPreview.isNull() || mPicture.isNull() || !area)
    {
        return;
    }
    // The picture, as large as fits, its shape kept.
    const LLRect r = area->getRect();
    const F32 aspect = (F32)mPicture->getWidth() / (F32)llmax(mPicture->getHeight(), 1);
    S32 w = r.getWidth();
    S32 h = ll_round((F32)w / aspect);
    if (h > r.getHeight())
    {
        h = r.getHeight();
        w = ll_round((F32)h * aspect);
    }
    const S32 x = r.mLeft + (r.getWidth() - w) / 2;
    const S32 y = r.mBottom + (r.getHeight() - h) / 2;
    gl_draw_scaled_image(x, y, w, h, mPreview);
    // For a texture cut to fit: what is cut away, dimmed.
    if (destination() == 1 && getChild<LLComboBox>("shape")->getValue().asInteger() == 0)
    {
        S32 tex_w, tex_h;
        LLRect part;
        textureShape(tex_w, tex_h, part);
        const F32 sx = (F32)w / mPicture->getWidth();
        const F32 sy = (F32)h / mPicture->getHeight();
        const S32 l = x + ll_round(part.mLeft * sx), rr = x + ll_round(part.mRight * sx);
        const S32 b = y + ll_round(part.mBottom * sy), t = y + ll_round(part.mTop * sy);
        const LLColor4 dim(0.f, 0.f, 0.f, 0.6f);
        gl_rect_2d(x, y + h, l, y, dim, true);
        gl_rect_2d(rr, y + h, x + w, y, dim, true);
        gl_rect_2d(l, y + h, rr, t, dim, true);
        gl_rect_2d(l, b, rr, y, dim, true);
    }
}

// Sent: the window closes, and a tip says so, and later how it went. A
// picture that cannot go -- no address, too large -- keeps the window open,
// saying why.
void LuminousRenderFloater::onSend()
{
    if (mPicture.isNull())
    {
        return;
    }
    LLUICtrl* status = getChild<LLUICtrl>("status");
    std::string location;
    LLAgentUI::buildLocationString(location, LLAgentUI::LOCATION_FORMAT_FULL);
    LLSLURL slurl;
    LLAgentUI::buildSLURL(slurl);
    switch (destination())
    {
        case 1:
        {
            // As Firestorm's snapshot window uploads a texture
            // (LLSnapshotLivePreview::saveTexture), with this window's shape,
            // name and description.
            LLPointer<LLImageRaw> texture = makeTexture();
            LLTransactionID tid;
            tid.generate();
            const LLAssetID asset = tid.makeAssetID(gAgent.getSecureSessionID());
            LLPointer<LLImageJ2C> j2c = new LLImageJ2C;
            if (!j2c->encode(texture, 0.f))
            {
                status->setValue("Could not encode the texture.");
                return;
            }
            LLFileSystem file(asset, LLAssetType::AT_TEXTURE, LLFileSystem::WRITE);
            file.write(j2c->getData(), j2c->getDataSize());
            const S32 cost = LLAgentBenefitsMgr::current().getTextureUploadCost(texture->getWidth(), texture->getHeight());
            LLResourceUploadInfo::ptr_t info = std::make_shared<LLResourceUploadInfo>(
                tid, LLAssetType::AT_TEXTURE, getChild<LLLineEditor>("tex_name")->getText(),
                getChild<LLLineEditor>("tex_desc")->getText(), 0, LLFolderType::FT_SNAPSHOT_CATEGORY,
                LLInventoryType::IT_SNAPSHOT, PERM_ALL, LLFloaterPerms::getGroupPerms("Uploads"),
                LLFloaterPerms::getEveryonePerms("Uploads"), cost, LLUUID::null, true);
            upload_new_resource(info);
            gViewerWindow->playSnapshotAnimAndSound();
            tell(llformat("Uploading a %d x %d texture to your Photo Album.", texture->getWidth(), texture->getHeight()));
            closeFloater();
            break;
        }
        case 2:
        {
            // Linden's upload takes a PNG only -- anything else it drops
            // without an answer, leaving "Posting" standing -- of at most
            // 8 MB: as large as fits, from 2048.
            LLPointer<LLImagePNG> png;
            for (S32 longest : { 2048, 1600, 1280, 1024 })
            {
                png = new LLImagePNG();
                if (!png->encode(scaledToFit(longest), 0.f))
                {
                    status->setValue("Could not encode the picture.");
                    return;
                }
                if (png->getDataSize() <= LLWebProfile::MAX_WEB_DATASIZE)
                {
                    break;
                }
            }
            if (png->getDataSize() > LLWebProfile::MAX_WEB_DATASIZE)
            {
                status->setValue("The picture is too large for the profile feed.");
                return;
            }
            // How it went, here; then the listener before ours (the
            // snapshot floater's) back, once this call is over.
            const LLWebProfile::status_callback_t before = LLWebProfile::getImageUploadResultCallback();
            LLWebProfile::setImageUploadResultCallback([before](bool ok)
            {
                tell(ok ? "Posted to your profile feed." : "Your profile feed did not take the picture.");
                doOnIdleOneTime([before]() { LLWebProfile::setImageUploadResultCallback(before); });
            });
            LLWebProfile::uploadImage(png.get(), getChild<LLTextEditor>("caption")->getText(),
                                      getChild<LLCheckBoxCtrl>("profile_location")->get());
            gViewerWindow->playSnapshotAnimAndSound();
            tell("Posting to your profile feed...");
            closeFloater();
            break;
        }
        case 3:
        {
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(95);
            if (!jpeg->encode(scaledToFit(4096), 0.f))
            {
                status->setValue("Could not encode the picture.");
                return;
            }
            LLSD params;
            params["title"] = getChild<LLLineEditor>("flickr_title")->getText();
            params["description"] = getChild<LLTextEditor>("flickr_desc")->getText();
            params["safety_level"] = getChild<LLComboBox>("flickr_rating")->getValue().asInteger();
            std::string tags;
            if (getChild<LLCheckBoxCtrl>("flickr_location")->get() && gAgent.getRegion())
            {
                tags = llformat("\"secondlife:region=%s\"", gAgent.getRegion()->getName().c_str());
            }
            params["tags"] = tags;
            LLFlickrConnect::instance().setConnectionState(LLFlickrConnect::FLICKR_POSTING);
            exoFlickr::uploadPhoto(params, jpeg.get(), [](bool success, const LLSD& response)
            {
                const bool ok = success && response["stat"].asString() == "ok";
                LLFlickrConnect::instance().setConnectionState(ok ? LLFlickrConnect::FLICKR_POSTED : LLFlickrConnect::FLICKR_POST_FAILED);
                if (ok)
                {
                    LLSD args;
                    args["ID"] = response["photoid"];
                    LLNotificationsUtil::add("ExodusFlickrUploadComplete", args); // with the link
                }
                else
                {
                    tell("Flickr did not take the picture.");
                }
            });
            tell("Posting to Flickr...");
            closeFloater();
            break;
        }
        case 4:
        {
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(95);
            if (!jpeg->encode(scaledToFit(4096), 0.f))
            {
                status->setValue("Could not encode the picture.");
                return;
            }
            static const char* const RATINGS[] = { "general", "moderate", "adult", "adult_plus" };
            LLSD params;
            params["rating"] = RATINGS[llclamp(getChild<LLComboBox>("primfeed_rating")->getValue().asInteger(), 1, 4) - 1];
            params["content"] = getChild<LLTextEditor>("primfeed_desc")->getText();
            params["is_commercial"] = false;
            params["post_to_public_gallery"] = getChild<LLCheckBoxCtrl>("primfeed_public")->get();
            if (getChild<LLCheckBoxCtrl>("primfeed_location")->get())
            {
                params["location"] = slurl.getSLURLString();
            }
            FSPrimfeedConnect::instance().setConnectionState(FSPrimfeedConnect::PRIMFEED_POSTING);
            FSPrimfeedConnect::instance().uploadPhoto(params, jpeg.get(), [](bool success, const std::string& url)
            {
                FSPrimfeedConnect::instance().setConnectionState(success ? FSPrimfeedConnect::PRIMFEED_POSTED
                                                                         : FSPrimfeedConnect::PRIMFEED_POST_FAILED);
                if (success)
                {
                    LLSD args;
                    args["PF_POSTURL"] = url;
                    LLNotificationsUtil::add("FSPrimfeedUploadComplete", args);
                }
                else
                {
                    tell("Primfeed did not take the picture.");
                }
            });
            tell("Posting to Primfeed...");
            closeFloater();
            break;
        }
        case 5:
        {
            const std::string to = getChild<LLLineEditor>("email_to")->getText();
            if (to.empty() || to.find('@') == std::string::npos)
            {
                status->setValue("Type an email address to send it to.");
                return;
            }
            std::string url = gAgent.getRegion() ? gAgent.getRegion()->getCapability("SendPostcard") : std::string();
            if (url.empty())
            {
                status->setValue("Postcards cannot be sent from this region.");
                return;
            }
            LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(90);
            if (!jpeg->encode(scaledToFit(1024), 0.f))
            {
                status->setValue("Could not encode the picture.");
                return;
            }
            LLResourceUploadInfo::ptr_t info = std::make_shared<LLPostcardUploadInfo>(
                std::string(), getChild<LLLineEditor>("email_from")->getText(), to,
                getChild<LLLineEditor>("email_subject")->getText(), getChild<LLTextEditor>("email_message")->getText(),
                gAgent.getPositionGlobal(), jpeg.get(),
                [to](LLUUID, LLUUID, LLUUID, LLSD response)
                {
                    tell(response["state"].asString() == "complete" ? "Sent to " + to + "." : "The email to " + to + " was not sent.");
                });
            LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            gViewerWindow->playSnapshotAnimAndSound();
            tell("Sending to " + to + "...");
            closeFloater();
            break;
        }
    }
}

// <Luminous> The render, over the viewer.

namespace
{
    // The world as last drawn, frozen while a render runs (made on demand,
    // and let go when the overlay closes: never a static GL object at exit).
    LLRenderTarget* sStill = nullptr;
    bool sWantStill = false;

    // Over the whole window while the render shows, above the menu bar and
    // the toolbars -- which are drawn above floaters, so the overlay cannot
    // cover them -- black, taking every click there, so nothing behind the
    // picture can be pressed. The middle, where the overlay is, it leaves
    // alone: clicks there go on to the overlay, a dialog or the render
    // window.
    class LuminousRenderShield : public LLView
    {
    public:
        LuminousRenderShield(const LLView::Params& p) : LLView(p) {}

        LLRect hole() const
        {
            LLRect hole;
            if (gFloaterView && getParent())
            {
                gFloaterView->localRectToOtherView(gFloaterView->getSnapRect(), &hole, getParent());
                hole.translate(-getRect().mLeft, -getRect().mBottom);
            }
            return hole;
        }

        void draw() override
        {
            const LLRect all = getLocalRect();
            const LLRect h = hole();
            const LLColor4 black(0.f, 0.f, 0.f, 1.f);
            gl_rect_2d(all.mLeft, all.mTop, all.mRight, h.mTop, black, true);       // above
            gl_rect_2d(all.mLeft, h.mBottom, all.mRight, all.mBottom, black, true); // below
            gl_rect_2d(all.mLeft, h.mTop, h.mLeft, h.mBottom, black, true);         // left
            gl_rect_2d(h.mRight, h.mTop, all.mRight, h.mBottom, black, true);       // right
        }

        bool outside(S32 x, S32 y) const { return !hole().pointInRect(x, y); }

        bool handleMouseDown(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleMouseUp(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleRightMouseDown(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleRightMouseUp(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleMiddleMouseDown(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleMiddleMouseUp(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleDoubleClick(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleScrollWheel(S32 x, S32 y, S32) override { return outside(x, y); }
        bool handleToolTip(S32 x, S32 y, MASK) override { return outside(x, y); }
        bool handleHover(S32 x, S32 y, MASK) override
        {
            if (!outside(x, y))
            {
                return false;
            }
            getWindow()->setCursor(UI_CURSOR_ARROW);
            return true;
        }
    };

    LuminousRenderShield* sShield = nullptr;
    // Firestorm's notification and IM icons sit inside the hole but are drawn
    // above every floater, the overlay too: hidden while the overlay is
    // open. New notices still come as toasts, which keep above it.
    bool sChicletsHidden = false;

    void showShield()
    {
        LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
        if (!root)
        {
            return;
        }
        // In main_view, over the toolbars but under its menus and tooltips:
        // in front of everything, it would hide the hints of the overlay's
        // own buttons, which fall below them onto the black.
        LLView* main_view = root->findChildView("main_view", false);
        LLView* holder = main_view ? main_view : root;
        if (LLPanel* chiclets = gViewerWindow->getChicletContainer(); chiclets && chiclets->getVisible())
        {
            chiclets->setVisible(false);
            sChicletsHidden = true;
        }
        if (!sShield)
        {
            LLView::Params p;
            p.name("luminous_render_shield");
            p.rect(holder->getLocalRect());
            p.follows.flags(FOLLOWS_ALL);
            // not opaque: an opaque view stops every click over it, the hole
            // too; outside the hole its handlers take them instead
            p.mouse_opaque(false);
            sShield = new LuminousRenderShield(p);
            holder->addChild(sShield);
        }
        holder->sendChildToFront(sShield);
        for (const char* above : { "Menu Holder", "tooltip view" }) // in main_view.xml's order
        {
            if (LLView* view = holder->findChildView(above, false))
            {
                holder->sendChildToFront(view);
            }
        }
        sShield->setVisible(true);
    }

    void hideShield()
    {
        if (sChicletsHidden)
        {
            if (LLPanel* chiclets = gViewerWindow ? gViewerWindow->getChicletContainer() : nullptr)
            {
                chiclets->setVisible(true);
            }
            sChicletsHidden = false;
        }
        if (sShield)
        {
            if (LLView* parent = sShield->getParent())
            {
                parent->removeChild(sShield);
            }
            delete sShield;
            sShield = nullptr;
        }
    }

    void releaseStill()
    {
        if (sStill)
        {
            sStill->release();
            delete sStill;
            sStill = nullptr;
        }
        sWantStill = false;
    }

    // From inside a render, which holds up the main loop: the whole interface
    // drawn once and shown, as display_startup draws it before login.
    void drawNow()
    {
        if (!gViewerWindow || !gViewerWindow->getWindow())
        {
            return;
        }
        gGL.flush();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        {
            LLGLSUIDefault gls_ui;
            gViewerWindow->setup2DRender();
            gViewerWindow->draw();
            gGL.flush();
        }
        LLVertexBuffer::unbind();
        gViewerWindow->getWindow()->swapBuffers();
    }
}

LuminousPictureView::LuminousPictureView(const LLSD& key)
    : LLFloater(key)
{
}

LuminousPictureView::~LuminousPictureView()
{
    release();
}

bool LuminousPictureView::postBuild()
{
    // It stays where it is: a floater's drag handle would let the picture be
    // dragged about like a window. Dragging inside it moves a zoomed-in
    // picture instead.
    setCanDrag(false);
    getChild<LLButton>("close")->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestClose(); });
    getChild<LLButton>("stop")->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestStop(); });
    getChild<LLButton>("share")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLView* menu = getChild<LLView>("share_panel");
        menu->setVisible(!menu->getVisible());
    });
    for (const char* what : { "download", "preview", "inventory", "profile", "flickr", "email" })
    {
        const std::string name = std::string("share_") + what;
        getChild<LLButton>(name)->setCommitCallback([this, what](LLUICtrl*, const LLSD&) { onShare(what); });
    }
    showButtons();
    return true;
}

LuminousPictureView* LuminousPictureView::instance()
{
    return LLFloaterReg::findTypedInstance<LuminousPictureView>("luminous_picture");
}

bool LuminousPictureView::rendering()
{
    LuminousPictureView* view = instance();
    return view && view->getVisible() && view->mState == State::Rendering;
}

void LuminousPictureView::beginRender()
{
    LuminousPictureView* view = LLFloaterReg::showTypedInstance<LuminousPictureView>("luminous_picture");
    if (!view)
    {
        return;
    }
    // Inside the toolbars; the shield blacks out and holds everything round
    // it, the toolbars and the menu bar included.
    view->fitToWindow();
    showShield();
    if (view->mTemporary && !view->mPath.empty())
    {
        LLFile::remove(view->mPath); // a render left open, given up for the new one
    }
    view->release();
    view->mPicture = nullptr;
    view->mPath.clear();
    view->mTemporary = false;
    view->mKept = false;
    view->mStopAsked = false;
    view->mState = State::Rendering;
    view->mProgress = -1.f;
    view->mProgressText = "Getting ready...";
    view->mNote.clear();
    view->showButtons();
    view->setFocus(true);
    sWantStill = true;
}

void LuminousPictureView::setProgress(F32 fraction, const std::string& text, bool draw_now)
{
    LuminousPictureView* view = instance();
    if (!view || view->mState != State::Rendering)
    {
        return;
    }
    view->mProgress = fraction;
    view->mProgressText = text;
    if (draw_now)
    {
        drawNow();
    }
}

void LuminousPictureView::showResult(const LLPointer<LLImageRaw>& picture, const std::string& path, bool temporary, bool light_only)
{
    LuminousPictureView* view = instance();
    if (!view)
    {
        if (temporary && !path.empty())
        {
            LLFile::remove(path);
        }
        return;
    }
    view->mState = State::Done;
    view->mPath = path;
    view->mTemporary = temporary;
    view->mLightOnly = light_only;
    view->mKept = !temporary;
    view->mPicture = picture;
    view->setPicture(picture);
    releaseStill();
    view->showButtons();
    view->setFocus(true);
}

void LuminousPictureView::setNote(const std::string& note)
{
    if (LuminousPictureView* view = instance())
    {
        view->mNote = note;
    }
}

// Over the picture's top, a dark box with the note in lines as wide as it
// allows.
void LuminousPictureView::drawNote(const LLRect& on) const
{
    if (mNote.empty())
    {
        return;
    }
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    const S32 width = llclamp(on.getWidth() - 80, 160, 640);
    std::vector<std::string> lines;
    std::string paragraph;
    std::istringstream paragraphs(mNote);
    while (std::getline(paragraphs, paragraph))
    {
        const LLWString text = utf8str_to_wstring(paragraph);
        S32 start = 0;
        while (start < (S32)text.size())
        {
            S32 count = font->maxDrawableChars(text.c_str() + start, (F32)width, (S32)text.size() - start,
                                               LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
            count = llmax(count, 1);
            lines.push_back(wstring_to_utf8str(text.substr(start, count)));
            start += count;
            while (start < (S32)text.size() && text[start] == ' ')
            {
                ++start;
            }
        }
    }
    const S32 line_h = font->getLineHeight() + 2;
    const S32 left = on.getCenterX() - width / 2;
    const S32 top = on.mTop - 16;
    gl_rect_2d(left - 12, top, left + width + 12, top - (S32)lines.size() * line_h - 16, LLColor4(0.f, 0.f, 0.f, 0.65f), true);
    S32 y = top - 8 - line_h;
    for (const std::string& line : lines)
    {
        font->renderUTF8(line, 0, (F32)left, (F32)y, LLColor4::white, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL,
                         LLFontGL::DROP_SHADOW);
        y -= line_h;
    }
}

void LuminousPictureView::endRender()
{
    if (LuminousPictureView* view = instance())
    {
        view->mState = State::Idle;
        view->closeFloater();
    }
    releaseStill();
}

bool LuminousPictureView::stopAsked()
{
    LuminousPictureView* view = instance();
    return view && view->mStopAsked;
}

bool LuminousPictureView::pollStop()
{
    LuminousPictureView* view = instance();
    if (!view || view->mState != State::Rendering)
    {
        return false;
    }
    if (!view->mStopAsked && gViewerWindow && gViewerWindow->getWindow())
    {
        std::vector<LLCoordGL> clicks;
        bool escape = false;
        gViewerWindow->getWindow()->takeHeldClicks(clicks, escape);
        bool stop = escape;
        LLView* button = view->getChild<LLView>("stop");
        const LLVector2 scale = gViewerWindow->getDisplayScale();
        for (const LLCoordGL& click : clicks)
        {
            // as LLViewerWindow takes a click: GL pixels to interface units,
            // then into the button
            S32 x = 0;
            S32 y = 0;
            button->screenPointToLocal(ll_round((F32)click.mX / scale.mV[VX]), ll_round((F32)click.mY / scale.mV[VY]), &x, &y);
            stop = stop || (button->getVisible() && button->getEnabled() && button->pointInView(x, y));
        }
        if (stop)
        {
            view->requestStop();
        }
    }
    return view->mStopAsked;
}

void LuminousPictureView::requestStop()
{
    if (mState != State::Rendering || mStopAsked)
    {
        return;
    }
    LL_INFOS("LuminousHDRSnapshot") << "Render stopped by the user" << LL_ENDL;
    mStopAsked = true;
    mProgressText = "Stopping...";
    showButtons();
}

bool LuminousPictureView::wantsStill()
{
    return sWantStill && rendering();
}

void LuminousPictureView::captureStill(LLRenderTarget* frame)
{
    sWantStill = false;
    if (!frame || !frame->isComplete())
    {
        return;
    }
    if (!sStill)
    {
        sStill = new LLRenderTarget();
    }
    if (sStill->getWidth() != frame->getWidth() || sStill->getHeight() != frame->getHeight())
    {
        sStill->release();
        // colour only: the frame's alpha is the glow mask, and drawn with it
        // the still would be dark where it is low, the sky black
        if (!sStill->allocate(frame->getWidth(), frame->getHeight(), GL_RGB))
        {
            releaseStill();
            return;
        }
    }
    gPipeline.copyRenderTarget(frame, sStill);
}

void LuminousPictureView::fitToWindow()
{
    if (!gFloaterView)
    {
        return;
    }
    const LLRect inside = gFloaterView->getSnapRect();
    const bool resized = inside.getWidth() != mFittedTo.getWidth() || inside.getHeight() != mFittedTo.getHeight();
    mFittedTo = inside;
    setShape(inside);
    placeButtons(LLRect(0, inside.getHeight(), inside.getWidth(), 0));
    if (resized)
    {
        mScale = 0.f; // the whole picture again, in the new size
    }
}

void LuminousPictureView::placeButtons(const LLRect& safe)
{
    // the overlay sits at the floater area's origin, so the safe part is in
    // its own coordinates
    mSafe = safe;
    placeOnPicture();
}

void LuminousPictureView::placeOnPicture()
{
    // Over the picture, in its bottom right corner, wherever it is drawn
    const LLRect on = onPicture();
    LLView* close = getChild<LLView>("close");
    LLView* share = getChild<LLView>("share");
    LLView* menu = getChild<LLView>("share_panel");
    close->setOrigin(on.mRight - 16 - close->getRect().getWidth(), on.mBottom + 16);
    getChild<LLView>("stop")->setOrigin(close->getRect().mLeft, close->getRect().mBottom); // in Close's place
    share->setOrigin(close->getRect().mLeft - 4 - share->getRect().getWidth(), on.mBottom + 16);
    menu->setOrigin(close->getRect().mRight - menu->getRect().getWidth(), share->getRect().mTop + 4);
}

LLRect LuminousPictureView::pictureRect() const
{
    const LLRect a = area();
    if (mState == State::Done && mTexture && mScale > 0.f)
    {
        const F32 left = (F32)a.getCenterX() - mCentreX * mScale;
        const F32 bottom = (F32)a.getCenterY() - mCentreY * mScale;
        return LLRect(ll_round(left), ll_round(bottom + (F32)mHeight * mScale), ll_round(left + (F32)mWidth * mScale), ll_round(bottom));
    }
    if (mState == State::Rendering)
    {
        // the still, as the picture will be: its shape, fitted
        const F32 aspect = llmax(LuminousHDRSnapshot::pictureAspect(), 0.01f);
        S32 w = a.getWidth();
        S32 h = ll_round((F32)w / aspect);
        if (h > a.getHeight())
        {
            h = a.getHeight();
            w = ll_round((F32)h * aspect);
        }
        const S32 left = a.mLeft + (a.getWidth() - w) / 2;
        const S32 bottom = a.mBottom + (a.getHeight() - h) / 2;
        return LLRect(left, bottom + h, left + w, bottom);
    }
    return a;
}

LLRect LuminousPictureView::onPicture() const
{
    // what shows of the picture inside the toolbars; with too little of it
    // for the buttons, all of the part inside the toolbars
    const LLRect safe = mSafe.isEmpty() ? area() : mSafe;
    LLRect on = pictureRect();
    on.intersectWith(safe);
    if (on.getWidth() < 240 || on.getHeight() < 120)
    {
        return safe;
    }
    return on;
}

void LuminousPictureView::showButtons()
{
    const bool done = mState == State::Done;
    getChild<LLView>("share")->setVisible(done);
    getChild<LLView>("close")->setVisible(done);
    getChild<LLView>("stop")->setVisible(mState == State::Rendering);
    getChild<LLUICtrl>("stop")->setEnabled(!mStopAsked);
    getChild<LLView>("share_panel")->setVisible(false);
    // a picture only in a temporary file can be downloaded; one on disk
    // already is
    getChild<LLUICtrl>("share_download")->setEnabled(done);
    getChild<LLUICtrl>("share_preview")->setEnabled(done && !mPath.empty());
    // An EXR's picture here is the light without the Look's tone: not one to
    // send on (render a PNG or JPEG for that).
    for (const char* name : { "share_inventory", "share_profile", "share_flickr", "share_email" })
    {
        getChild<LLUICtrl>(name)->setEnabled(done && !mLightOnly);
    }
}

void LuminousPictureView::onShare(const std::string& what)
{
    getChild<LLView>("share_panel")->setVisible(false);
    if (mState != State::Done || mPicture.isNull())
    {
        return;
    }
    if (what == "download")
    {
        // A copy of the file, where the dialog says; its folder remembered,
        // as for renders to disk.
        const std::string from = mPath;
        LLHandle<LLFloater> handle = getHandle();
        LLFilePickerReplyThread::startPicker(
            [from, handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
            {
                if (files.empty() || from.empty())
                {
                    return;
                }
                if (LLFile::copy(from, files[0]))
                {
                    gSavedPerAccountSettings.setString("SnapshotBaseDir", gDirUtilp->getDirName(files[0]));
                    if (LuminousPictureView* view = dynamic_cast<LuminousPictureView*>(handle.get()))
                    {
                        view->mKept = true;
                    }
                    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", "Saved " + files[0]));
                }
                else
                {
                    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", "Could not save " + files[0]));
                }
            },
            LLFilePicker::FFSAVE_ALL, gDirUtilp->getBaseFileName(from));
        return;
    }
    if (what == "preview")
    {
        if (!mPath.empty() && gViewerWindow && gViewerWindow->getWindow())
        {
            gViewerWindow->getWindow()->openFile(mPath);
        }
        return;
    }
    // On to one of Second Life's places, in the render window, which keeps
    // its own copy of the picture.
    const U32 destination = what == "inventory" ? 1 : what == "profile" ? 2 : what == "flickr" ? 3 : what == "primfeed" ? 4 : 5;
    mKept = true;
    LuminousRenderFloater::showPicture(mPicture, destination);
}

void LuminousPictureView::requestClose()
{
    if (mState == State::Rendering)
    {
        return; // it closes itself when the render is done
    }
    if (mState == State::Done && !mKept)
    {
        LLHandle<LLFloater> handle = getHandle();
        LLNotificationsUtil::add("GenericAlertYesCancel",
            LLSD().with("MESSAGE", "Close the render? It has not been downloaded or sent anywhere, so it will be gone."), LLSD(),
            [handle](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                {
                    if (LLFloater* view = handle.get())
                    {
                        view->closeFloater();
                    }
                }
            });
        return;
    }
    closeFloater();
}

void LuminousPictureView::release()
{
    if (mTexture)
    {
        glDeleteTextures(1, &mTexture);
        mTexture = 0;
    }
    mWidth = mHeight = 0;
}

void LuminousPictureView::onClose(bool app_quitting)
{
    release(); // a large picture is hundreds of megabytes on the GPU
    releaseStill();
    hideShield();
    if (mTemporary && !mPath.empty())
    {
        LLFile::remove(mPath);
    }
    mPath.clear();
    mTemporary = false;
    mPicture = nullptr;
    mState = State::Idle;
    LLFloater::onClose(app_quitting);
}

void LuminousPictureView::setPicture(const LLPointer<LLImageRaw>& picture)
{
    release();
    mScale = 0.f;
    if (picture.isNull() || picture->isBufferInvalid() || picture->getWidth() < 1 || picture->getHeight() < 1)
    {
        return;
    }
    // As large as the GPU takes (16384 on Apple silicon: every render fits).
    LLPointer<LLImageRaw> image = picture;
    const S32 most = llmax(gGLManager.mGLMaxTextureSize, 1024);
    if (image->getWidth() > most || image->getHeight() > most || (image->getComponents() != 3 && image->getComponents() != 4))
    {
        image = new LLImageRaw(picture->getData(), picture->getWidth(), picture->getHeight(), picture->getComponents());
        const F32 shrink = llmin(1.f, (F32)most / (F32)llmax(image->getWidth(), image->getHeight()));
        if (shrink < 1.f)
        {
            image->scale(llmax(1, (S32)(image->getWidth() * shrink)), llmax(1, (S32)(image->getHeight() * shrink)));
        }
        if (image->getComponents() != 3 && image->getComponents() != 4)
        {
            LLPointer<LLImageRaw> rgb = new LLImageRaw(image->getWidth(), image->getHeight(), 3);
            rgb->copy(image);
            image = rgb;
        }
    }
    mWidth = image->getWidth();
    mHeight = image->getHeight();
    const bool alpha = image->getComponents() == 4;
    // A texture of its own: any size, with mip levels so the whole picture
    // shrinks cleanly; bottom row first, as images are.
    glGenTextures(1, &mTexture);
    gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // the viewer relies on 1 (LLRender::init)
    glTexImage2D(GL_TEXTURE_2D, 0, alpha ? GL_RGBA8 : GL_RGB8, mWidth, mHeight, 0, alpha ? GL_RGBA : GL_RGB,
                 GL_UNSIGNED_BYTE, image->getData());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

LLRect LuminousPictureView::area() const
{
    // Centred on the window, not on the part inside the toolbars, which a
    // toolbar on one side pushes off centre: the largest rect about the
    // window's middle that still lies inside the overlay.
    const LLRect all = getLocalRect();
    LLView* root = gViewerWindow ? gViewerWindow->getRootView() : nullptr;
    if (!root)
    {
        return all;
    }
    LLRect window;
    root->localRectToOtherView(root->getLocalRect(), &window, this);
    const S32 cx = llclamp(window.getCenterX(), all.mLeft, all.mRight);
    const S32 cy = llclamp(window.getCenterY(), all.mBottom, all.mTop);
    const S32 half_w = llmin(cx - all.mLeft, all.mRight - cx);
    const S32 half_h = llmin(cy - all.mBottom, all.mTop - cy);
    if (half_w < 50 || half_h < 50)
    {
        return all;
    }
    return LLRect(cx - half_w, cy + half_h, cx + half_w, cy - half_h);
}

F32 LuminousPictureView::fitScale() const
{
    const LLRect a = area();
    return llmax(llmin((F32)a.getWidth() / (F32)llmax(mWidth, 1), (F32)a.getHeight() / (F32)llmax(mHeight, 1)), 1e-4f);
}

F32 LuminousPictureView::fullScale() const
{
    // a UI unit is getScaleFactor() screen pixels (two on Retina)
    return 1.f / llmax(LLUI::getScaleFactor().mV[VX], 0.1f);
}

void LuminousPictureView::clampCentre()
{
    const LLRect a = area();
    const F32 half_w = (F32)a.getWidth() * 0.5f / mScale;
    const F32 half_h = (F32)a.getHeight() * 0.5f / mScale;
    // a picture narrower than the view stays in its middle; a wider one
    // cannot be moved past its edges
    mCentreX = (F32)mWidth <= 2.f * half_w ? (F32)mWidth * 0.5f : llclamp(mCentreX, half_w, (F32)mWidth - half_w);
    mCentreY = (F32)mHeight <= 2.f * half_h ? (F32)mHeight * 0.5f : llclamp(mCentreY, half_h, (F32)mHeight - half_h);
}

void LuminousPictureView::zoomAbout(F32 scale, S32 x, S32 y)
{
    // the picture's point under (x, y) stays there
    const LLRect a = area();
    const F32 dx = (F32)(x - a.getCenterX());
    const F32 dy = (F32)(y - a.getCenterY());
    const F32 px = mCentreX + dx / mScale;
    const F32 py = mCentreY + dy / mScale;
    mScale = scale;
    mCentreX = px - dx / mScale;
    mCentreY = py - dy / mScale;
    clampCentre();
}

void LuminousPictureView::draw()
{
    // Over every other floater, the sidebar included, so that no click meant
    // for the render reaches what is under it -- but under the render window
    // it sends the picture on through, and under dialogs and notices (toasts
    // are modal dialogs). Put right between frames: reordering the floaters
    // while they are being drawn would pull the list from under the loop.
    // The window resized: the overlay follows, with its buttons, between
    // frames as well, so the buttons stay in sight.
    if (gFloaterView && !mRefitting && gFloaterView->getSnapRect() != mFittedTo)
    {
        mRefitting = true;
        LLHandle<LLFloater> handle = getHandle();
        doOnIdleOneTime([handle]()
        {
            if (LuminousPictureView* view = dynamic_cast<LuminousPictureView*>(handle.get()))
            {
                view->mRefitting = false;
                if (view->getVisible())
                {
                    view->fitToWindow();
                }
            }
        });
    }
    if (gFloaterView && !mRaising)
    {
        LuminousRenderFloater* sender = LLFloaterReg::findTypedInstance<LuminousRenderFloater>("luminous_render");
        const bool sending = sender && sender->getVisible();
        LLFloater* front = gFloaterView->getFrontmost();
        if (front && front != this && !(sending && front == sender) && !dynamic_cast<LLModalDialog*>(front))
        {
            mRaising = true;
            LLHandle<LLFloater> handle = getHandle();
            doOnIdleOneTime([handle]()
            {
                LuminousPictureView* view = dynamic_cast<LuminousPictureView*>(handle.get());
                if (!view || !gFloaterView)
                {
                    return;
                }
                view->mRaising = false;
                if (!view->getVisible())
                {
                    return;
                }
                // with the keyboard, for Esc -- unless the render window is
                // open over it, which keeps it
                LuminousRenderFloater* sender = LLFloaterReg::findTypedInstance<LuminousRenderFloater>("luminous_render");
                const bool sending = sender && sender->getVisible();
                gFloaterView->bringToFront(view, !sending);
                if (sending)
                {
                    gFloaterView->bringToFront(sender, true);
                }
            });
        }
    }
    const LLRect a = area();
    if (mState == State::Rendering)
    {
        // The world, frozen; until it is caught, the world itself shows.
        if (sStill && sStill->isComplete())
        {
            gl_rect_2d(getLocalRect(), LLColor4::black, true);
            // As the picture will be: cut to its shape -- the render keeps the
            // view's height and its own width -- and fitted as the picture is
            // when done, so the one takes the other's place exactly.
            const F32 still = (F32)sStill->getWidth() / (F32)llmax(sStill->getHeight(), 1U);
            const F32 aspect = llmax(LuminousHDRSnapshot::pictureAspect(), 0.01f);
            LLRectf uv(0.f, 1.f, 1.f, 0.f);
            if (aspect < still)
            {
                const F32 keep = aspect / still;
                uv.mLeft = 0.5f - keep * 0.5f;
                uv.mRight = 0.5f + keep * 0.5f;
            }
            else if (aspect > still)
            {
                const F32 keep = still / aspect;
                uv.mBottom = 0.5f - keep * 0.5f;
                uv.mTop = 0.5f + keep * 0.5f;
            }
            const LLRect still_rect = pictureRect();
            LLGLDisable blend(GL_BLEND);
            gl_draw_scaled_rotated_image(still_rect.mLeft, still_rect.mBottom, still_rect.getWidth(), still_rect.getHeight(), 0.f,
                                         nullptr, LLColor4::white, uv, sStill);
        }
        // How far it is: a slim bar near the picture's bottom, the step
        // above it, clear of Stop on the right.
        const LLRect safe = onPicture();
        const S32 bar_w = llmax(llmin(360, safe.getWidth() - 2 * 130), 60);
        const S32 left = safe.getCenterX() - bar_w / 2;
        const S32 bottom = safe.mBottom + 36;
        gl_rect_2d(left - 10, bottom + 36, left + bar_w + 10, bottom - 10, LLColor4(0.f, 0.f, 0.f, 0.55f), true);
        gl_rect_2d(left, bottom + 4, left + bar_w, bottom, LLColor4(1.f, 1.f, 1.f, 0.25f), true);
        if (mProgress >= 0.f)
        {
            gl_rect_2d(left, bottom + 4, left + ll_round(bar_w * llclamp(mProgress, 0.f, 1.f)), bottom, LLColor4::white, true);
        }
        else
        {
            // not known how long: a short piece going to and fro
            const F32 t = fmodf((F32)LLTimer::getElapsedSeconds(), 2.f);
            const F32 pos = t < 1.f ? t : 2.f - t;
            const S32 piece = bar_w / 4;
            const S32 x = left + ll_round((bar_w - piece) * pos);
            gl_rect_2d(x, bottom + 4, x + piece, bottom, LLColor4::white, true);
        }
        LLFontGL::getFontSansSerif()->renderUTF8(mProgressText, 0, (F32)safe.getCenterX(), (F32)(bottom + 12), LLColor4::white,
                                                 LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        drawNote(safe);
        placeOnPicture();
        LLFloater::draw();
        return;
    }
    gl_rect_2d(getLocalRect(), LLColor4::black, true);
    if (mTexture)
    {
        if (mScale <= 0.f)
        {
            mScale = fitScale();
            mCentreX = (F32)mWidth * 0.5f;
            mCentreY = (F32)mHeight * 0.5f;
        }
        clampCentre();
        const F32 left = (F32)a.getCenterX() - mCentreX * mScale;
        const F32 bottom = (F32)a.getCenterY() - mCentreY * mScale;
        const F32 right = left + (F32)mWidth * mScale;
        const F32 top = bottom + (F32)mHeight * mScale;
        {
            LLLocalClipRect clip(getLocalRect()); // zoomed in, all of the overlay
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
            // past 100%, the pixels themselves, as an editor shows them
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mScale > fullScale() * 1.01f ? GL_NEAREST : GL_LINEAR);
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            gGL.begin(LLRender::TRIANGLES);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(left, bottom);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2f(right, bottom);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(right, top);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex2f(left, bottom);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2f(right, top);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2f(left, top);
            gGL.end();
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        }
        // how large, bottom left
        std::string size = llformat("%d%%   %d x %d", ll_round(mScale / fullScale() * 100.f), mWidth, mHeight);
        if (mLightOnly)
        {
            size += "   (an EXR: the light, without the Look's tone)";
        }
        const LLRect safe = onPicture(); // over the picture, bottom left
        LLFontGL::getFontSansSerifSmall()->renderUTF8(size, 0, (F32)(safe.mLeft + 10), (F32)(safe.mBottom + 22), LLColor4::white,
                                                      LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        drawNote(safe);
    }
    placeOnPicture();
    LLFloater::draw(); // the buttons
}

bool LuminousPictureView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (mState != State::Done || !mTexture || mScale <= 0.f)
    {
        return true; // nothing under the overlay is scrolled
    }
    // from the whole picture up to 400%
    const F32 least = llmin(fitScale(), fullScale());
    const F32 most = fullScale() * 4.f;
    zoomAbout(llclamp(mScale * powf(1.25f, (F32)-clicks), least, most), x, y);
    return true;
}

bool LuminousPictureView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (childrenHandleMouseDown(x, y, mask))
    {
        return true; // a button
    }
    getChild<LLView>("share_panel")->setVisible(false);
    if (mState == State::Done && mTexture)
    {
        mDragging = true;
        mLastX = x;
        mLastY = y;
        gFocusMgr.setMouseCapture(this);
    }
    return true; // the overlay takes every click
}

bool LuminousPictureView::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging && hasMouseCapture())
    {
        mCentreX -= (F32)(x - mLastX) / mScale;
        mCentreY -= (F32)(y - mLastY) / mScale;
        mLastX = x;
        mLastY = y;
        clampCentre();
        return true;
    }
    return LLFloater::handleHover(x, y, mask);
}

bool LuminousPictureView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
        return true;
    }
    return LLFloater::handleMouseUp(x, y, mask);
}

bool LuminousPictureView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mState == State::Done && mTexture && !childrenHandleDoubleClick(x, y, mask))
    {
        // the whole picture, or 100% where clicked
        if (mScale < fullScale() * 0.99f)
        {
            zoomAbout(fullScale(), x, y);
        }
        else
        {
            mScale = fitScale();
            mCentreX = (F32)mWidth * 0.5f;
            mCentreY = (F32)mHeight * 0.5f;
        }
        return true;
    }
    return LLFloater::handleDoubleClick(x, y, mask);
}

bool LuminousPictureView::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        if (mState == State::Rendering)
        {
            requestStop();
        }
        else
        {
            requestClose();
        }
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}
// </Luminous>

