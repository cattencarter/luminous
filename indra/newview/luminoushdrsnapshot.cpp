/**
 * @file luminoushdrsnapshot.cpp
 * @brief Snapshots with more than eight bits: the light as OpenEXR, the
 * finished picture as 16-bit TIFF.
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

#include "luminoushdrsnapshot.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lltexturefetch.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llimagebmp.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llnotificationsutil.h"
#include "llrendertarget.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"
#include "llviewercamera.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "luminousphotomode.h"
#include "luminousfloater.h"
#include "llmotioncontroller.h"
#include "llcharacter.h"
#include "llheadrotmotion.h"
#include "llwindow.h"
#include "pipeline.h"
#include "fsfloaterprimfeed.h"
#include "llfloaterflickr.h"
#include "llfloaterreg.h"
#include "llfloatersnapshot.h"
#include "llsidetraypanelcontainer.h"
#include "llsnapshotlivepreview.h"
#include "llmeshrepository.h"
#include "llvoavatar.h"
#include "llvovolume.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>
#include <new>
#if LL_DARWIN
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace LuminousHDRSnapshot
{
    namespace
    {
        // A frame read back from the GPU: RGB floats, bottom row first, as
        // OpenGL gives them.
        struct Frame
        {
            U32 width = 0;
            U32 height = 0;
            std::vector<F32> rgb;
        };

        // Set while rawSnapshot renders; capture() reads each frame as it is
        // drawn and adds it to the sum.
        bool sArmed = false;
        EFormat sFormat = EXR;
        Frame sFrame;          // the sum of the frames captured so far
        S32 sFrameCount = 0;

        // Frames averaged as light: each frame, just before tonemapping, is
        // added at 1 / frames into a float target, and the last frame's is
        // replaced by the sum, so the picture is tonemapped (and read) once,
        // from the average, as a camera would. In a sum of finished pictures
        // a light brighter than white is already cut to white, and an
        // average of frames seen through different points of the lens loses
        // the brightness that opens highlights into discs.
        bool sAverage = false;
        S32 sAverageFrame = 0;
        S32 sAverageFrames = 0;
        LLRenderTarget sAverageTarget;

        // False if it could not: then the frames are captured and summed one
        // by one.
        bool averageFrame(LLRenderTarget* hdr_screen)
        {
            LLGLSLShader& shader = gLuminousAverageProgram;
            if (!shader.isComplete())
            {
                return false;
            }
            const U32 w = hdr_screen->getWidth();
            const U32 h = hdr_screen->getHeight();
            if (sAverageTarget.getWidth() != w || sAverageTarget.getHeight() != h)
            {
                sAverageTarget.release();
                // 32-bit float: a sixteen-bit TIFF needs more than half
                // float's eleven bits, summed over up to 64 frames, and an
                // EXR keeps the light for grading. An eight-bit picture is
                // summed in half float, 8 bytes a pixel less (150 MB at twice
                // a 2560-wide window): 16 frames' rounding stays under half
                // an 8-bit step (LuminousSnapshotAverageFloat32 to compare).
                static LLCachedControl<bool> float32(gSavedSettings, "LuminousSnapshotAverageFloat32", false);
                const U32 format = eightBit(sFormat) && !float32() ? GL_RGBA16F : GL_RGBA32F;
                if (!sAverageTarget.allocate(w, h, format))
                {
                    sAverageTarget.release();
                    return false;
                }
            }
            static LLStaticHashedString s_scale("luminous_scale");
            LLGLDisable depth(GL_DEPTH_TEST);
            sAverageTarget.bindTarget();
            if (sAverageFrame == 0)
            {
                GLfloat was[4];
                glGetFloatv(GL_COLOR_CLEAR_VALUE, was);
                glClearColor(0.f, 0.f, 0.f, 0.f);
                sAverageTarget.clear();
                glClearColor(was[0], was[1], was[2], was[3]);
            }
            {
                LLGLEnable blend(GL_BLEND);
                gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE);
                shader.bind();
                shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, hdr_screen, false, LLTexUnit::TFO_POINT);
                shader.uniform1f(s_scale, 1.f / (F32)llmax(sAverageFrames, 1));
                gPipeline.mScreenTriangleVB->setBuffer();
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                shader.unbind();
            }
            sAverageTarget.flush();
            if (sAverageFrame >= sAverageFrames - 1)
            {
                LLGLDisable blend(GL_BLEND);
                hdr_screen->bindTarget();
                // Colour only: the frame's alpha is read further on, and
                // written as 1 it makes the picture many times too bright.
                gGL.setColorMask(true, false);
                shader.bind();
                shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &sAverageTarget, false, LLTexUnit::TFO_POINT);
                shader.uniform1f(s_scale, 1.f);
                gPipeline.mScreenTriangleVB->setBuffer();
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                shader.unbind();
                gGL.setColorMask(true, true);
                hdr_screen->flush();
            }
            gGL.setSceneBlendType(LLRender::BT_ALPHA);
            return true;
        }

        // Adds one frame to the sum.
        void accumulate(Frame& one)
        {
            if (sFrame.rgb.empty() || sFrame.width != one.width || sFrame.height != one.height)
            {
                sFrame = std::move(one);
                sFrameCount = 1;
                return;
            }
            for (size_t i = 0; i < sFrame.rgb.size(); ++i)
            {
                sFrame.rgb[i] += one.rgb[i];
            }
            ++sFrameCount;
        }
        std::string sCaptureError;

        bool readFrame(LLRenderTarget* target, Frame& frame)
        {
            frame.width = target->getWidth();
            frame.height = target->getHeight();
            try
            {
                frame.rgb.assign((size_t)frame.width * frame.height * 3, 0.f);
            }
            catch (const std::bad_alloc&)
            {
                frame.rgb.clear();
                return false;
            }
            // Rows of RGB floats are whole multiples of any pack alignment, so
            // the viewer's own (1, from LLRender::init) is left as it is.
            target->bindTarget();
            glReadPixels(0, 0, frame.width, frame.height, GL_RGB, GL_FLOAT, frame.rgb.data());
            target->flush();
            return true;
        }

        // Round-to-nearest-even float to IEEE half, clamped to half's finite
        // range: a highlight brighter than 65504 stays the brightest white
        // rather than becoming infinity.
        U16 toHalf(F32 value)
        {
            if (!(value >= 0.f))
            {
                value = 0.f; // negative and NaN
            }
            value = llmin(value, 65504.f);
            U32 bits;
            memcpy(&bits, &value, sizeof(bits));
            S32 exponent = (S32)((bits >> 23) & 0xff) - 127 + 15;
            U32 mantissa = bits & 0x7fffff;
            if (exponent <= 0)
            {
                if (exponent < -10)
                {
                    return 0;
                }
                mantissa |= 0x800000;
                const U32 shift = (U32)(14 - exponent);
                U32 half = mantissa >> shift;
                const U32 rest = mantissa & ((1u << shift) - 1);
                const U32 midpoint = 1u << (shift - 1);
                if (rest > midpoint || (rest == midpoint && (half & 1)))
                {
                    ++half;
                }
                return (U16)half;
            }
            U32 half = ((U32)exponent << 10) | (mantissa >> 13);
            const U32 rest = mantissa & 0x1fff;
            if (rest > 0x1000 || (rest == 0x1000 && (half & 1)))
            {
                ++half; // a carry into the exponent is still the right value
            }
            return (U16)llmin(half, 0x7bffu);
        }

        // The output, top row first: each pixel the average of the frame's
        // pixels under it, after cropping the frame's centre to the output's
        // shape -- which is what rawSnapshot does when it reads back.
        void reduce(const Frame& frame, U32 render_width, U32 render_height, U32 out_width, U32 out_height,
                    std::vector<F32>& out)
        {
            const F32 ratio = llmin((F32)frame.width / render_width, (F32)frame.height / render_height);
            const F32 crop_w = ratio * render_width;
            const F32 crop_h = ratio * render_height;
            const F32 x0 = ((F32)frame.width - crop_w) * 0.5f;
            const F32 y0 = ((F32)frame.height - crop_h) * 0.5f;
            const F32 sx = crop_w / out_width;
            const F32 sy = crop_h / out_height;

            out.assign((size_t)out_width * out_height * 3, 0.f);
            for (U32 y = 0; y < out_height; ++y)
            {
                const F32 top = y0 + crop_h - y * sy;
                const F32 bottom = top - sy;
                S32 j0 = (S32)ceilf(bottom - 0.5f);
                S32 j1 = (S32)ceilf(top - 0.5f) - 1;
                if (j1 < j0)
                {
                    j0 = j1 = (S32)floorf((top + bottom) * 0.5f);
                }
                j0 = llclamp(j0, 0, (S32)frame.height - 1);
                j1 = llclamp(j1, 0, (S32)frame.height - 1);
                for (U32 x = 0; x < out_width; ++x)
                {
                    const F32 left = x0 + x * sx;
                    const F32 right = left + sx;
                    S32 i0 = (S32)ceilf(left - 0.5f);
                    S32 i1 = (S32)ceilf(right - 0.5f) - 1;
                    if (i1 < i0)
                    {
                        i0 = i1 = (S32)floorf((left + right) * 0.5f);
                    }
                    i0 = llclamp(i0, 0, (S32)frame.width - 1);
                    i1 = llclamp(i1, 0, (S32)frame.width - 1);

                    F32 sum[3] = { 0.f, 0.f, 0.f };
                    for (S32 j = j0; j <= j1; ++j)
                    {
                        const F32* row = &frame.rgb[((size_t)j * frame.width) * 3];
                        for (S32 i = i0; i <= i1; ++i)
                        {
                            for (S32 c = 0; c < 3; ++c)
                            {
                                const F32 v = row[i * 3 + c];
                                sum[c] += std::isfinite(v) ? v : 0.f;
                            }
                        }
                    }
                    const F32 n = (F32)((j1 - j0 + 1) * (i1 - i0 + 1));
                    F32* px = &out[((size_t)y * out_width + x) * 3];
                    for (S32 c = 0; c < 3; ++c)
                    {
                        px[c] = sum[c] / n;
                    }
                }
            }
        }

        template <typename T>
        void put(std::string& buffer, T value)
        {
            buffer.append(reinterpret_cast<const char*>(&value), sizeof(T)); // little-endian, as both formats here are
        }

        void putAttribute(std::string& header, const char* name, const char* type, const std::string& value)
        {
            header.append(name).push_back('\0');
            header.append(type).push_back('\0');
            put<S32>(header, (S32)value.size());
            header.append(value);
        }

        // OpenEXR, single part, scan lines, no compression, half-float B, G, R
        // (channels in alphabetical order, as the format wants). With no
        // chromaticities attribute the primaries are Rec. 709 with a D65 white
        // -- the viewer's linear sRGB.
        bool writeEXR(const std::string& path, U32 width, U32 height, const std::vector<F32>& rgb, F32 scale)
        {
            std::string header;
            put<S32>(header, 20000630);
            put<S32>(header, 2);

            std::string channels;
            for (const char* name : { "B", "G", "R" })
            {
                channels.append(name).push_back('\0');
                put<S32>(channels, 1); // HALF
                put<U8>(channels, 0);  // pLinear
                put<U8>(channels, 0);
                put<U8>(channels, 0);
                put<U8>(channels, 0);
                put<S32>(channels, 1); // x sampling
                put<S32>(channels, 1); // y sampling
            }
            channels.push_back('\0');
            putAttribute(header, "channels", "chlist", channels);
            putAttribute(header, "compression", "compression", std::string(1, '\0'));
            std::string box;
            put<S32>(box, 0);
            put<S32>(box, 0);
            put<S32>(box, (S32)width - 1);
            put<S32>(box, (S32)height - 1);
            putAttribute(header, "dataWindow", "box2i", box);
            putAttribute(header, "displayWindow", "box2i", box);
            putAttribute(header, "lineOrder", "lineOrder", std::string(1, '\0'));
            std::string one;
            put<F32>(one, 1.f);
            putAttribute(header, "pixelAspectRatio", "float", one);
            std::string centre;
            put<F32>(centre, 0.f);
            put<F32>(centre, 0.f);
            putAttribute(header, "screenWindowCenter", "v2f", centre);
            putAttribute(header, "screenWindowWidth", "float", one);
            header.push_back('\0');

            const U64 line_bytes = (U64)width * 3 * sizeof(U16);
            const U64 first_line = header.size() + (U64)height * sizeof(U64);
            for (U32 y = 0; y < height; ++y)
            {
                put<U64>(header, first_line + y * (8 + line_bytes));
            }

            LLFILE* file = LLFile::fopen(path, "wb");
            if (!file)
            {
                return false;
            }
            bool ok = fwrite(header.data(), 1, header.size(), file) == header.size();
            std::vector<U16> line((size_t)width * 3);
            for (U32 y = 0; ok && y < height; ++y)
            {
                const F32* row = &rgb[(size_t)y * width * 3];
                for (U32 x = 0; x < width; ++x)
                {
                    line[x] = toHalf(row[x * 3 + 2] * scale);             // B
                    line[width + x] = toHalf(row[x * 3 + 1] * scale);     // G
                    line[2 * width + x] = toHalf(row[x * 3 + 0] * scale); // R
                }
                std::string block;
                put<S32>(block, (S32)y);
                put<S32>(block, (S32)line_bytes);
                ok = fwrite(block.data(), 1, block.size(), file) == block.size()
                  && fwrite(line.data(), 1, line_bytes, file) == line_bytes;
            }
            ok = (fclose(file) == 0) && ok;
            return ok;
        }

        // Baseline TIFF, one strip, uncompressed RGB at sixteen bits a sample.
        bool writeTIFF16(const std::string& path, U32 width, U32 height, const std::vector<F32>& rgb)
        {
            const U32 image_bytes = width * height * 3 * 2;
            const U16 entries = 12;
            const U32 ifd = 8;
            const U32 bits_at = ifd + 2 + entries * 12 + 4;
            const U32 xres_at = bits_at + 6;
            const U32 yres_at = xres_at + 8;
            const U32 image_at = yres_at + 8;

            std::string head;
            head.append("II");
            put<U16>(head, 42);
            put<U32>(head, ifd);
            put<U16>(head, entries);
            auto entry = [&head](U16 tag, U16 type, U32 count, U32 value)
            {
                put<U16>(head, tag);
                put<U16>(head, type);
                put<U32>(head, count);
                if (type == 3 && count == 1)
                {
                    put<U16>(head, (U16)value);
                    put<U16>(head, 0);
                }
                else
                {
                    put<U32>(head, value);
                }
            };
            entry(256, 4, 1, width);          // ImageWidth
            entry(257, 4, 1, height);         // ImageLength
            entry(258, 3, 3, bits_at);        // BitsPerSample 16, 16, 16
            entry(259, 3, 1, 1);              // no compression
            entry(262, 3, 1, 2);              // RGB
            entry(273, 4, 1, image_at);       // StripOffsets
            entry(277, 3, 1, 3);              // SamplesPerPixel
            entry(278, 4, 1, height);         // RowsPerStrip
            entry(279, 4, 1, image_bytes);    // StripByteCounts
            entry(282, 5, 1, xres_at);        // XResolution
            entry(283, 5, 1, yres_at);        // YResolution
            entry(296, 3, 1, 2);              // inches
            put<U32>(head, 0);                // no next directory
            put<U16>(head, 16);
            put<U16>(head, 16);
            put<U16>(head, 16);
            put<U32>(head, 72);
            put<U32>(head, 1);
            put<U32>(head, 72);
            put<U32>(head, 1);

            LLFILE* file = LLFile::fopen(path, "wb");
            if (!file)
            {
                return false;
            }
            bool ok = fwrite(head.data(), 1, head.size(), file) == head.size();
            std::vector<U16> line((size_t)width * 3);
            for (U32 y = 0; ok && y < height; ++y)
            {
                const F32* row = &rgb[(size_t)y * width * 3];
                for (U32 i = 0; i < width * 3; ++i)
                {
                    line[i] = (U16)ll_round(llclamp(row[i], 0.f, 1.f) * 65535.f);
                }
                ok = fwrite(line.data(), 1, line.size() * 2, file) == line.size() * 2;
            }
            ok = (fclose(file) == 0) && ok;
            return ok;
        }

        F32 sExposure = 1.f;

        // Set while a render from the Render image button is saved into a
        // temporary file: an eight-bit copy of it to show over the viewer,
        // made as the file is written.
        LLPointer<LLImageRaw>* sShow = nullptr;
        // Set while a render from the Render image button runs: its frames
        // report how far it is to the overlay, which draws it at once.
        bool sProgress = false;
        // A preview renders (renderPreview), and its moment held after it
        // while its overlay shows: the playback speed to give back.
        bool sPreview = false;
        bool sShapedRender = false; // a render in a shape other than the window's (shapedRender)
        bool sPreviewHold = false;
        F32 sPreviewHeldFactor = 1.f;

        // How a render is shown: only saved (asked for with a path: LEAP,
        // Phototools' buttons); saved to disk with the overlay's progress
        // (Render to disk); or kept in a temporary file and shown when done,
        // to download or send on.
        enum EShow { SAVE_ONLY, TO_DISK, TEMPORARY };

        std::string defaultName(EFormat format)
        {
            char stamp[32];
            time_t now = time(nullptr);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", localtime(&now));
            const char* extension = format == EXR ? ".exr" : format == TIFF16 ? ".tif"
                                  : format == JPEG8 ? ".jpg" : format == BMP8 ? ".bmp" : ".png";
            return std::string("Luminous_") + stamp + extension;
        }

        void report(const std::string& message)
        {
            LL_INFOS("LuminousHDRSnapshot") << message << LL_ENDL;
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
        }

        bool saveTo(EFormat format, const std::string& path, EShow show)
        {
            std::string error;
            LLPointer<LLImageRaw> shown;
            sShow = show == TEMPORARY ? &shown : nullptr;
            sProgress = show != SAVE_ONLY;
            const bool saved = takeAndSave(format, path, error);
            if (sProgress)
            {
                LuminousPictureView::pollStop(); // clicks made while it saved: dropped
            }
            sShow = nullptr;
            sProgress = false;
            if (!saved)
            {
                const bool stopped = show != SAVE_ONLY && LuminousPictureView::stopAsked();
                if (show != SAVE_ONLY)
                {
                    LuminousPictureView::endRender();
                }
                if (show == TEMPORARY && LLFile::isfile(path)) // a stop leaves none
                {
                    LLFile::remove(path);
                }
                if (!stopped && show != SAVE_ONLY) // stopped: the overlay going is answer enough
                {
                    // A window over the viewer: a line in nearby chat would
                    // be hidden behind the overlay as it closes
                    LL_INFOS("LuminousHDRSnapshot") << "Could not save the render: " << error << LL_ENDL;
                    LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Could not finish the render: " + error));
                }
                else if (!stopped)
                {
                    report("Could not save the snapshot: " + error);
                }
                return false;
            }
            if (show == TEMPORARY)
            {
                // Shown over the viewer, to download or send on; the file is
                // removed when the overlay closes.
                LuminousPictureView::showResult(shown, path, true, format == EXR);
                return true;
            }
            if (show == TO_DISK)
            {
                LuminousPictureView::endRender();
            }
            report("Saved " + path);
            // In its default app as well, if asked for.
            static LLCachedControl<bool> open(gSavedSettings, "LuminousSnapshotOpen", false);
            if (open && gViewerWindow && gViewerWindow->getWindow())
            {
                gViewerWindow->getWindow()->openFile(path);
            }
            return true;
        }

        bool sWaiting = false;
        bool sFullTextures = false;

        // Every snapshot says first that it will take a while, and waits a
        // moment so that the notice is drawn before the viewer pauses to
        // render. With LuminousSnapshotFullTextures -- "Full detail" -- the
        // textures on screen load as finely as the picture needs
        // (fullDetailAreaScale; every texture at full size would run out of
        // memory in a busy region), objects and avatars are drawn at their
        // highest detail however far off (the LOD factors at their maxima,
        // for the shot only), and the shot waits -- three seconds at least,
        // thirty at most -- until neither textures nor meshes have been
        // loading for one. Meanwhile the reflection probes, which update one
        // at a time every frame, take in the new detail.
        void whenSharp(std::function<void()> act, bool overlay, std::function<void()> on_stop = nullptr);

        // The viewer's memory as macOS counts it, what macOS could still
        // give it, and its memory pressure (below).
        U64 footprintBytes();
        U64 availableBytes();
        S32 memoryPressure();
        // What a memory message suggests besides the Render tab.
        const char* const QUIT_OTHER_APPS =
            " Quitting apps you are not using frees memory for it: right-click each in the Dock and choose Quit"
            " (Preview with large pictures open can hold a lot).";
        U64 footprintMB()
        {
            return footprintBytes() / (1024 * 1024);
        }

        // Meshes being fetched or decoded: headers, levels of detail, the
        // queue not yet sent.
        bool meshesLoading()
        {
            if (LLMeshRepoThread::sActiveHeaderRequests > 0 || LLMeshRepoThread::sActiveLODRequests > 0
                || LLMeshRepository::sLODProcessing > 0)
            {
                return true;
            }
            LLMutexLock lock(gMeshRepo.mMeshMutex);
            if (!gMeshRepo.mPendingRequests.empty())
            {
                return true;
            }
            for (const auto& loading : gMeshRepo.mLoadingMeshes)
            {
                if (!loading.empty())
                {
                    return true;
                }
            }
            return false;
        }

        // Animations held still or let go again: avatars' and the motion
        // controller's time, as the Playback speed setting sets it.
        void setAnimationTime(F32 factor)
        {
            LLMotionController::setCurrentTimeFactor(factor);
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                character->setAnimTimeFactor(factor);
            }
        }

        void saveWhenSharp(EFormat format, const std::string& path, EShow show)
        {
            if (show == SAVE_ONLY)
            {
                whenSharp([format, path, show]() { saveTo(format, path, show); }, false);
                return;
            }
            if (sWaiting)
            {
                report("A snapshot is already being taken.");
                return;
            }
            // The moment of the click is the one rendered, as a camera's: every
            // animation held from now until the render is done, so the still
            // shown meanwhile and the picture show the same pose. A render
            // from a preview's overlay takes over the preview's hold: the
            // same moment, and the playback speed it found.
            const bool preview = sPreview;
            const F32 time_factor = sPreviewHold ? sPreviewHeldFactor : LLMotionController::getCurrentTimeFactor();
            sPreviewHold = false;
            setAnimationTime(0.f);
            LuminousPictureView::beginRender(); // the world frozen, how far it is
            whenSharp([format, path, show, time_factor, preview]()
            {
                struct LetGo
                {
                    F32 factor;
                    bool keep;
                    ~LetGo() { if (!keep) setAnimationTime(factor); }
                } let_go{ time_factor, false };
                struct EndPreview
                {
                    bool on;
                    ~EndPreview() { if (on) sPreview = false; }
                } end_preview{ preview };
                if (saveTo(format, path, show) && preview)
                {
                    // held while the preview shows, for its Render
                    let_go.keep = true;
                    sPreviewHold = true;
                    sPreviewHeldFactor = time_factor;
                }
            }, true,
            [time_factor, preview]() // stopped before it rendered
            {
                setAnimationTime(time_factor);
                if (preview)
                {
                    sPreview = false;
                }
            });
        }

        void whenSharp(std::function<void()> act, bool overlay, std::function<void()> on_stop)
        {
            static LLCachedControl<bool> full(gSavedSettings, "LuminousSnapshotFullTextures", false);
            static LLCachedControl<U32> frames(gSavedSettings, "LuminousSnapshotFrames", 16);
            if (sWaiting)
            {
                report("A snapshot is already being taken.");
                return;
            }
            sWaiting = true;
            const bool use_full = full && !sPreview; // a preview takes the textures as they are
            sFullTextures = use_full;
            // Every avatar's gaze held from now to the end: Second Life's own
            // glances aside and blinks would otherwise be in one picture in a
            // few.
            LLEyeMotion::sHoldGaze = true;
            // Highest detail for the shot: the LOD factors as their settings'
            // handlers set them, put back after (LLViewerControl's handlers
            // for RenderVolumeLODFactor and RenderAvatarLODFactor).
            const F32 volume_lod = LLVOVolume::sLODFactor;
            const F32 volume_distance = LLVOVolume::sDistanceFactor;
            const F32 avatar_lod = LLVOAvatar::sLODFactor;
            if (use_full)
            {
                LLVOVolume::sLODFactor = MAX_LOD_FACTOR;
                LLVOVolume::sDistanceFactor = 1.f - LLVOVolume::sLODFactor * 0.1f;
                LLVOAvatar::sLODFactor = MAX_AVATAR_LOD_FACTOR;
            }
            if (!overlay) // the overlay says how far it is instead
            {
                report(use_full ? "Taking the snapshot: loading everything in full detail first. This takes several seconds, and the viewer pauses while it renders."
                            : (frames() > 1 ? "Taking the snapshot. This takes a few seconds, and the viewer pauses while it renders."
                                            : "Taking the snapshot..."));
            }
            else if (use_full)
            {
                LuminousPictureView::setProgress(-1.f, "Loading everything in full detail...", false);
            }
            const bool wait_for_textures = use_full;
            const U64 start_mb = footprintMB();
            LLTimer started;
            LLTimer quiet;
            doOnIdleRepeating([=]() mutable
            {
                // Not begun at all after a quit or a teleport during the wait:
                // it would draw the teleport screen, or hold up the logout.
                const bool quitting = LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent();
                const bool moving = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE || !gAgent.getRegion();
                if ((overlay && LuminousPictureView::stopAsked()) || quitting || moving)
                {
                    if (moving && !quitting)
                    {
                        report("The render was stopped: a teleport began while it waited.");
                    }
                    // Stop while it waited for full detail: nothing rendered
                    if (wait_for_textures)
                    {
                        LLVOVolume::sLODFactor = volume_lod;
                        LLVOVolume::sDistanceFactor = volume_distance;
                        LLVOAvatar::sLODFactor = avatar_lod;
                    }
                    sFullTextures = false;
                    sWaiting = false;
                    LLEyeMotion::sHoldGaze = false;
                    LuminousPictureView::endRender();
                    if (on_stop)
                    {
                        on_stop();
                    }
                    return true;
                }
                const F32 elapsed = started.getElapsedTimeF32();
                if (!wait_for_textures)
                {
                    if (elapsed < 0.3f)
                    {
                        return false; // let the notice be drawn
                    }
                }
                else
                {
                    if (LLAppViewer::getTextureFetch()->getNumRequests() > 0 || meshesLoading())
                    {
                        quiet.reset();
                    }
                    // Three seconds at least: the objects' detail is weighed
                    // again only every few frames, and only then are the
                    // finer meshes asked for.
                    if (elapsed < 3.f || (quiet.getElapsedTimeF32() < 1.f && elapsed < 30.f))
                    {
                        return false; // call again
                    }
                    // with the memory it took: what Full detail costs in a busy region
                    LL_INFOS("LuminousHDRSnapshot") << "Full detail after " << elapsed << " s, the viewer at "
                                                    << start_mb << " MB when asked, " << footprintMB() << " MB now" << LL_ENDL;
                }
                if (overlay)
                {
                    LuminousPictureView::setProgress(0.f, sPreview ? "Rendering a preview..." : "Rendering...", true);
                }
                try
                {
                    act();
                }
                catch (const std::bad_alloc&)
                {
                    LuminousPhotoMode::setAccumulationFrame(-1);
                    LuminousPhotoMode::setSnapshotShadows(false);
                    sArmed = false;
                    sShow = nullptr;
                    sProgress = false;
                    if (overlay)
                    {
                        LuminousPictureView::endRender();
                    }
                    report("Could not save the snapshot: not enough memory.");
                }
                if (wait_for_textures)
                {
                    LLVOVolume::sLODFactor = volume_lod;
                    LLVOVolume::sDistanceFactor = volume_distance;
                    LLVOAvatar::sLODFactor = avatar_lod;
                }
                sFullTextures = false;
                sWaiting = false;
                LLEyeMotion::sHoldGaze = sPreviewHold; // a preview's moment held, gaze too
                return true;
            });
        }

        void save(EFormat format, const LLSD& param, EShow show = SAVE_ONLY)
        {
            std::string path = param.asString();
            if (!path.empty())
            {
                saveWhenSharp(format, path, show);
                return;
            }
            // Next to Firestorm's own snapshots, once a folder has been chosen
            // for them; else ask.
            std::string dir = gSavedPerAccountSettings.getString("SnapshotBaseDir");
            if (!dir.empty() && LLFile::isdir(dir))
            {
                saveWhenSharp(format, gDirUtilp->add(dir, defaultName(format)), show);
                return;
            }
            LLFilePickerReplyThread::startPicker(
                [format, show](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
                {
                    if (!files.empty())
                    {
                        // The folder is remembered, so the next render does
                        // not ask again; it is Firestorm's own snapshot
                        // folder, changed on the Render tab.
                        gSavedPerAccountSettings.setString("SnapshotBaseDir", gDirUtilp->getDirName(files[0]));
                        saveWhenSharp(format, files[0], show);
                    }
                },
                LLFilePicker::FFSAVE_ALL, defaultName(format));
        }
    }

    void capture(LLRenderTarget* hdr_screen, LLRenderTarget* exposure_map)
    {
        if (sAverage && !LuminousPhotoMode::warmingUp())
        {
            if (!averageFrame(hdr_screen))
            {
                // No float target at this size: each frame is captured on
                // its own from here (the frames already summed, if any, are
                // lost).
                LL_WARNS("LuminousHDRSnapshot") << "Frames not averaged as light from frame " << sAverageFrame
                                                << ": no " << hdr_screen->getWidth() << "x" << hdr_screen->getHeight()
                                                << " float target" << LL_ENDL;
                sAverage = false;
            }
            else if (sAverageFrame < sAverageFrames - 1)
            {
                return; // only the last frame, the average, is the picture
            }
        }
        if (!sArmed || LuminousPhotoMode::warmingUp())
        {
            return;
        }
        sCaptureError.clear();

        // The exposure Firestorm's toneMap applies: RenderExposure times the
        // metered exposure, and the Look's own on top.
        F32 metered = 1.f;
        exposure_map->bindTarget();
        glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &metered);
        exposure_map->flush();
        static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);
        sExposure = llclamp(exposure(), 0.5f, 4.f) * metered;
        if (LuminousPhotoMode::look())
        {
            static LLCachedControl<F32> look_ev(gSavedSettings, "LuminousPhotoExposure", 0.f);
            sExposure *= powf(2.f, llclamp(look_ev(), -5.f, 5.f));
        }

        Frame one;
        if (sFormat == EXR)
        {
            if (!readFrame(hdr_screen, one))
            {
                sCaptureError = "not enough memory to read the frame";
                return;
            }
            accumulate(one);
            return;
        }

        // The finished picture: Firestorm's own tonemap pass, Look and all,
        // into a 32-bit float target -- half float holds only eleven bits near
        // white, too few for a sixteen-bit file. Averaged frames tonemap into
        // the averaging target itself: its sum is back in the frame, and a
        // second float target at the picture's size would be 16 bytes a
        // pixel more (1.2 GB at twice a 5K window).
        LLRenderTarget finished;
        LLRenderTarget* target = &finished;
        if (sAverage && sAverageTarget.getWidth() == hdr_screen->getWidth() && sAverageTarget.getHeight() == hdr_screen->getHeight())
        {
            target = &sAverageTarget;
        }
        else if (!finished.allocate(hdr_screen->getWidth(), hdr_screen->getHeight(), GL_RGBA32F))
        {
            sCaptureError = "could not make a 16-bit target";
            return;
        }
        gPipeline.tonemap(hdr_screen, target, true);
        if (!readFrame(target, one))
        {
            sCaptureError = "not enough memory to read the frame";
        }
        else
        {
            accumulate(one);
        }
        finished.release();
    }

    namespace
    {
        // Set while takePicture renders: the PNG path keeps its image here
        // instead of writing a file.
        LLPointer<LLImageRaw>* sKeep = nullptr;
    }

    namespace
    {
        // The viewer's whole footprint as macOS counts it, compressed and
        // swapped memory included (LLMemory counts only what is resident);
        // 0 where unknown.
        U64 footprintBytes()
        {
#if LL_DARWIN
            task_vm_info_data_t info;
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
            {
                return info.phys_footprint;
            }
#endif
            return 0;
        }

        // What macOS could give the viewer without taking it from other
        // apps: free memory, and what it keeps but can drop -- inactive,
        // speculative and purgeable pages; 0 where unknown. So quitting
        // other apps (Preview with large pictures open holds a lot) leaves
        // a render more room.
        U64 availableBytes()
        {
#if LL_DARWIN
            vm_statistics64_data_t vm;
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
            if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
            {
                return ((U64)vm.free_count + vm.inactive_count + vm.speculative_count + vm.purgeable_count) * (U64)vm_kernel_page_size;
            }
#endif
            return 0;
        }

        // macOS's memory pressure: 1 normal, 2 warning, 4 critical; 0
        // where unknown.
        S32 memoryPressure()
        {
#if LL_DARWIN
            int level = 0;
            size_t size = sizeof(level);
            if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &size, nullptr, 0) == 0)
            {
                return level;
            }
#endif
            return 0;
        }
    }

    bool takePicture(LLPointer<LLImageRaw>& picture, std::string& error)
    {
        sKeep = &picture;
        const bool ok = takeAndSave(PNG8, std::string(), error);
        sKeep = nullptr;
        return ok && picture.notNull();
    }

    bool takeAndSave(EFormat format, const std::string& path, std::string& error)
    {
        // The window's size, in pixels, or twice it.
        static LLCachedControl<U32> scale_setting(gSavedSettings, "LuminousSnapshotScale", 1);
        const U32 asked_scale = sPreview ? 1U : llclamp((U32)scale_setting(), 1U, 2U); // what fits may be less (below)
        static LLCachedControl<bool> smooth(gSavedSettings, "LuminousHDRSnapshotSmooth", true);
        static LLCachedControl<U32> frames_setting(gSavedSettings, "LuminousSnapshotFrames", 16);
        // A preview takes four: enough for smooth edges and hair.
        const S32 frames = sPreview ? 4 : (S32)llclamp((U32)frames_setting(), 1U, 256U);

        static LLCachedControl<bool> hdr(gSavedSettings, "RenderHDREnabled", true);
        if (!hdr && !eightBit(format)) // the eight-bit picture is the finished one, HDR or not
        {
            error = "HDR rendering is off";
            return false;
        }

        // One render, never tiles: the frame is read once, before tonemapping.
        // As large as the GPU's largest texture (16384 on Apple silicon).
        const U32 most = (U32)gGLManager.mGLMaxTextureSize;
        static LLCachedControl<bool> margin_setting(gSavedSettings, "LuminousSnapshotMargin", true);
        struct Sizes
        {
            U32 out_w, out_h;         // the picture as saved
            U32 picture_w, picture_h; // as rendered, before any smoothing reduces it
            U32 margin_x, margin_y;   // the frame round it
            U32 render_w, render_h;
        };
        const bool margin_wanted = margin_setting() && LuminousPhotoMode::isOn() && !sPreview;
        auto sizes_for = [&](F32 scale, bool margin)
        {
            Sizes z;
            pictureSize(scale, z.out_w, z.out_h);
            // An eight-bit picture is read after anti-aliasing, and an
            // accumulated snapshot's frames are shifted by fractions of a
            // pixel, so neither needs doubling.
            const U32 factor = !eightBit(format) && frames == 1 && smooth && z.out_w * 2 <= most && z.out_h * 2 <= most ? 2 : 1;
            z.picture_w = z.out_w * factor;
            z.picture_h = z.out_h * factor;
            // ... and the frame round it: a tenth more on each side
            // (LuminousSnapshotMargin), the view widened to match, so that
            // what looks at its neighbours on screen -- reflections, GTAO,
            // contact shadows, depth of field, bloom -- finds
            // something there at the picture's edges. Less where the GPU's
            // largest texture would not hold it, none where the memory
            // would not (below).
            z.margin_x = 0;
            z.margin_y = 0;
            if (margin && margin_wanted)
            {
                const F32 share = llmin(0.1f, llmin(((F32)most / z.picture_w - 1.f) * 0.5f, ((F32)most / z.picture_h - 1.f) * 0.5f));
                if (share > 0.01f)
                {
                    z.margin_y = (U32)ll_round(z.picture_h * share);
                    z.margin_x = (U32)ll_round(z.picture_w * (F32)z.margin_y / (F32)z.picture_h);
                }
            }
            z.render_w = z.picture_w + 2 * z.margin_x;
            z.render_h = z.picture_h + 2 * z.margin_y;
            return z;
        };

        // What fits in memory, chosen before anything is made: the size asked
        // with photo mode's shadow maps, else with smaller ones, else the
        // largest size between it and the window's that fits -- a picture
        // rather than a failure.
        // A render costs some bytes a pixel above the scene -- the pipeline's
        // targets at its size and Luminous's own -- which depend on the
        // effects on: about 140 with all of them on, less with fewer.
        // LuminousRenderBytesPerPixel starts at 140 and is learnt from each
        // render (below); TIFF and EXR read floats back, 20 more. Then 256 MB,
        // and the shadow maps whatever the size.
        // The limit: a share of the Mac's memory (LuminousSnapshotMemoryShare,
        // three quarters), and no more than the viewer has now and macOS
        // could still give it, less 0.5 GB for macOS and the rest -- so other
        // apps' memory counts, and quitting them leaves more room.
        static LLCachedControl<F32> memory_share(gSavedSettings, "LuminousSnapshotMemoryShare", 0.75f);
        U64 memory_limit = (U64)((F64)gSysMemory.getPhysicalMemoryKB().value() * 1024.0 * llclamp((F64)memory_share(), 0.05, 0.95));
        const U64 footprint_now = footprintBytes();
        const U64 available_now = availableBytes();
        const U64 reserve = 512ULL << 20;
        if (footprint_now > 0 && available_now > 0)
        {
            memory_limit = llmin(memory_limit, footprint_now + (available_now > reserve ? available_now - reserve : 0));
        }
        static LLCachedControl<U32> shadow_setting(gSavedSettings, "LuminousPhotoShadowMapSize", 8192);
        static LLCachedControl<S32> shadow_detail(gSavedSettings, "RenderShadowDetail", 2);
        static LLCachedControl<F32> bytes_setting(gSavedSettings, "LuminousRenderBytesPerPixel", 140.f);
        const F64 bytes_a_pixel = llclamp((F64)bytes_setting(), 60.0, 300.0) + (eightBit(format) ? 0.0 : 20.0);
        const U32 full_shadow = llclamp((U32)shadow_setting(), 1024U, llmax(most, 1024U));
        const U32 small_shadow = llmin(full_shadow, 4096U);
        static LLCachedControl<F32> shadow_scale(gSavedSettings, "RenderShadowResolutionScale", 1.f);
        // The shadow maps the render makes, less the live view's that they
        // replace: those are in the footprint already, and are given back
        // before the first frame (below).
        auto shadow_bytes = [&](const Sizes& z, U32 shadow) -> S64
        {
            S64 bytes = 0;
            if (shadow_detail() > 0 && LuminousPhotoMode::isOn()) // the sun's four
            {
                const LLRenderTarget& live = gPipeline.mRT->shadow[0];
                bytes += 4LL * shadow * shadow * 4 - 4LL * live.getWidth() * live.getHeight() * 4;
            }
            // the two projectors', at the frame's width times the shadow
            // scale, no larger than the sun's (LLPipeline::allocateShadowBuffer)
            // -- only if a projector casts shadows: an empty slot's map is
            // never drawn or read, so it takes no memory (bindShadowMaps)
            if (shadow_detail() > 1 && (gPipeline.mShadowSpotLight[0].notNull() || gPipeline.mShadowSpotLight[1].notNull()))
            {
                const S64 spot = (S64)llmin((U64)(z.render_w * llmax((F32)shadow_scale(), 0.f)), (U64)shadow);
                const LLRenderTarget* live = gPipeline.getSpotShadowTarget(0);
                bytes += 2LL * spot * spot * 4 - (live ? 2LL * live->getWidth() * live->getHeight() * 4 : 0);
            }
            return bytes;
        };
        auto need_bytes = [&](const Sizes& z, U32 shadow) -> U64
        {
            const S64 need = (S64)((F64)z.render_w * z.render_h * bytes_a_pixel) + (256LL << 20) + shadow_bytes(z, shadow);
            return (U64)llmax(need, (S64)(256LL << 20));
        };
        struct Option
        {
            F32 scale;
            U32 shadow;
            bool margin;
        };
        // The size asked as it is; with 6144 maps between 8192 and 4096 (half
        // a GB less than 8192, the picture the same within noise); then
        // without the margin (about 780 MB less at twice a 2560-wide window:
        // what lies just outside the picture is not seen, but the picture
        // keeps its size); then with 4096 maps, which take almost a GB more
        // off and soften shadows a little; then smaller sizes a quarter of the
        // window apart down to the window's.
        const U32 mid_shadow = full_shadow > 6144U && small_shadow < 6144U ? 6144U : full_shadow;
        std::vector<Option> options = { { (F32)asked_scale, full_shadow, true } };
        if (mid_shadow < full_shadow)
        {
            options.push_back({ (F32)asked_scale, mid_shadow, true });
        }
        if (margin_wanted)
        {
            options.push_back({ (F32)asked_scale, mid_shadow, false });
        }
        if (small_shadow < mid_shadow)
        {
            options.push_back({ (F32)asked_scale, small_shadow, false });
        }
        for (F32 scale = (F32)asked_scale - 0.25f; scale > 0.99f; scale -= 0.25f)
        {
            options.push_back({ scale, small_shadow, false });
        }
        Option chosen = options.front(); // unread memory: as asked, the limit between frames still holds
        bool fits = true;
        if (footprint_now > 0)
        {
            chosen = options.back(); // none fits: the smallest, and the limit between frames decides
            fits = false;
            for (const Option& option : options)
            {
                if (footprint_now + need_bytes(sizes_for(option.scale, option.margin), option.shadow) <= memory_limit)
                {
                    chosen = option;
                    fits = true;
                    break;
                }
            }
            LL_INFOS("LuminousHDRSnapshot") << "Memory: the viewer at " << footprint_now / (1024 * 1024) << " MB of "
                << memory_limit / (1024 * 1024) << " MB allowed (macOS could give " << available_now / (1024 * 1024)
                << " MB more); the render as asked needs about "
                << need_bytes(sizes_for((F32)asked_scale, true), full_shadow) / (1024 * 1024) << " MB more at "
                << (S32)ll_round(bytes_a_pixel) << " bytes a pixel; rendering at " << chosen.scale
                << " times the window's size with " << chosen.shadow << " shadow maps"
                << (margin_wanted && !chosen.margin ? ", without the margin" : "") << "; projectors casting shadows: "
                << (gPipeline.mShadowSpotLight[0].notNull() ? 1 : 0) + (gPipeline.mShadowSpotLight[1].notNull() ? 1 : 0) << LL_ENDL;
        }
        const U64 free_bytes = memory_limit > footprint_now ? memory_limit - footprint_now : 0;
        const F64 free_gb = (F64)free_bytes / (1024.0 * 1024.0 * 1024.0);
        if (chosen.scale >= (F32)asked_scale && margin_wanted && !chosen.margin && fits)
        {
            // The size asked, without the margin: said as a smaller size is
            const F64 need_gb = (F64)need_bytes(sizes_for((F32)asked_scale, true), mid_shadow) / (1024.0 * 1024.0 * 1024.0);
            const std::string what = llformat("This render is drawn without the margin round the picture, to fit: with it, it needs about %.1f GB, and this Mac has %.1f GB free for it with this region loaded. Reflections, blur and glow at the picture's very edges see less beyond them.",
                                              need_gb, free_gb);
            LL_INFOS("LuminousHDRSnapshot") << "Rendering without the margin: with it needs about " << need_gb << " GB, " << free_gb << " GB free" << LL_ENDL;
            if (sProgress)
            {
                LuminousPictureView::setNote(what
                    + "\nFor the margin, quit apps you are not using -- right-click each in the Dock and choose Quit (Preview with large pictures open can hold a lot) -- and render again."
                    + (sFullTextures ? " Turning off Full detail on the Render tab helps too." : ""));
            }
            else
            {
                report(what + QUIT_OTHER_APPS);
            }
        }
        if (chosen.scale < (F32)asked_scale && fits) // not fitting at all, the render's own message says so
        {
            // what it would take at the size asked: without the margin, with the smaller maps
            const Sizes asked = sizes_for((F32)asked_scale, false);
            const Sizes taken = sizes_for(chosen.scale, false);
            const std::string how = chosen.scale > 1.01f ? llformat("%g times the window's size", chosen.scale) : std::string("the window's size");
            const F64 need_gb = (F64)need_bytes(asked, small_shadow) / (1024.0 * 1024.0 * 1024.0);
            if (sProgress)
            {
                // On the render itself, while it renders and on the
                // picture after: a line in nearby chat would lie behind
                // the overlay
                LL_INFOS("LuminousHDRSnapshot") << "Rendering at " << how << " (" << taken.out_w << " x " << taken.out_h << ") instead of "
                    << asked.out_w << " x " << asked.out_h << ": needs about " << need_gb << " GB, " << free_gb << " GB free" << LL_ENDL;
                LuminousPictureView::setNote(
                    llformat("This render is %s (%u x %u), not twice it (%u x %u): twice it needs about %.1f GB, and this Mac has %.1f GB free for it with this region loaded.",
                             how.c_str(), taken.out_w, taken.out_h, asked.out_w, asked.out_h, need_gb, free_gb)
                    + "\nFor the full size, quit apps you are not using -- right-click each in the Dock and choose Quit (Preview with large pictures open can hold a lot) -- and render again."
                    + (sFullTextures ? " Turning off Full detail on the Render tab helps too." : ""));
            }
            else
            {
                report(llformat("Rendering at %s (%u x %u): twice it (%u x %u) needs about %.1f GB, and this Mac has %.1f GB free for it with this region loaded.",
                                how.c_str(), taken.out_w, taken.out_h, asked.out_w, asked.out_h, need_gb, free_gb)
                       + QUIT_OTHER_APPS);
            }
        }
        const Sizes sizes = sizes_for(chosen.scale, chosen.margin);
        const S64 render_shadow_bytes = shadow_bytes(sizes, chosen.shadow); // before the maps are made, for the learning below
        const U32 out_w = sizes.out_w;
        const U32 out_h = sizes.out_h;
        const U32 picture_w = sizes.picture_w;
        const U32 picture_h = sizes.picture_h;
        const U32 margin_y = sizes.margin_y;
        const U32 render_w = sizes.render_w;
        const U32 render_h = sizes.render_h;
        const F32 frame_scale = (F32)render_h / (F32)picture_h;

        // At full resolution, whatever the live view renders at: held for all
        // the frames, so the targets are resized once each way, and the first
        // rawSnapshot draws one warm-up frame. The world view as rawSnapshot
        // will set it (no interface: the whole window) first, so that the
        // hold sizes the targets for it.
        gViewerWindow->updateWorldViewRect(true);
        LuminousPhotoMode::FullResolution full_resolution;
        // A picture larger than the window: its targets kept from frame to frame.
        LuminousPhotoMode::KeepSnapshotTargets keep_targets;

        sFrame = Frame();
        sFrameCount = 0;
        sCaptureError.clear();
        sFormat = format;
        sArmed = !eightBit(format);
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        raw->enableOverSize(); // past 256 MB, as Firestorm's high-res snapshot
        std::vector<F32> png_sum;
        S32 png_frames = 0; // in png_sum: all, or those after averaging as light failed
        bool rendered = true;
        // The frames, each shifted by a fraction of a pixel and with fresh
        // noise, rendered back to back: the scene does not move between them,
        // as the viewer does no other work meanwhile.
        // Photo mode's snapshot shadow maps: allocated for the shot, and given
        // back after it.
        const bool big_shadows = LuminousPhotoMode::isOn();
        if (big_shadows)
        {
            LuminousPhotoMode::setSnapshotShadows(true, chosen.shadow);
            gPipeline.allocateShadowBuffer(gPipeline.mRT->screen.getWidth(), gPipeline.mRT->screen.getHeight());
        }
        // What that and the full resolution gave back -- the live view's
        // maps and targets, drawn -- deleted now, not after the first frame:
        // the main loop that would delete it waits for the render. No
        // texture unit may still think one of them bound.
        LLImageGL::flushDeletedTextures();
        for (U32 unit = 0; unit < LL_NUM_TEXTURE_LAYERS; ++unit)
        {
            gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
        }
        gGL.getTexUnit(0)->activate();
        // A render that would take more than a share of the Mac's memory
        // (LuminousSnapshotMemoryShare, three quarters; memory_limit above)
        // stops between frames, with a message, rather than push the Mac
        // into swap.
        bool too_big = false;
        bool stopped = false;
        LuminousPhotoMode::setAccumulationFrames(frames); // for the lens and sun sampled across them
        // The margin: the view widened by the frame's extra height; put back after.
        LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 view = camera->getView();
        if (margin_y > 0)
        {
            camera->setViewNoBroadcast(2.f * atanf(tanf(view * 0.5f) * frame_scale));
            LuminousPhotoMode::setFrameMargin(frame_scale);
        }
        // Averaged on the GPU as light, EXR too; with the setting off, EXR
        // reads every frame back as floats and sums them here.
        static LLCachedControl<bool> as_light(gSavedSettings, "LuminousSnapshotAverageAsLight", true);
        sAverage = as_light && frames > 1 && gLuminousAverageProgram.isComplete();
        sAverageFrames = frames;
        // Everything the frames change, put back once they are done, on
        // every way out -- an exception from inside a frame included, which
        // is caught further out (whenSharp) by code that knows none of this.
        struct PutBack
        {
            LLViewerCamera* camera;
            F32 view;
            bool big_shadows;
            bool done = false;
            void now()
            {
                if (done)
                {
                    return;
                }
                done = true;
                LuminousPhotoMode::setAccumulationFrame(-1);
                LuminousPhotoMode::setAccumulationFrames(0);
                LuminousPhotoMode::setFrameMargin(1.f);
                camera->setViewNoBroadcast(view);
                sArmed = false;
                sAverage = false;
                sShapedRender = false;
                sAverageTarget.release();
                // the effects' own targets, sized to the render: those drawn
                // live are made again at the live view's size
                LuminousPhotoMode::releaseRenderSizedTargets();
                if (big_shadows)
                {
                    LuminousPhotoMode::setSnapshotShadows(false);
                    gPipeline.allocateShadowBuffer(gPipeline.mRT->screen.getWidth(), gPipeline.mRT->screen.getHeight());
                }
                // Given back now, before the picture is encoded, as after
                // each frame: no unit may still think one of them bound.
                LLImageGL::flushDeletedTextures();
                for (U32 unit = 0; unit < LL_NUM_TEXTURE_LAYERS; ++unit)
                {
                    gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
                }
                gGL.getTexUnit(0)->activate();
            }
            ~PutBack()
            {
                now();
            }
        } put_back{ camera, view, big_shadows };
        // A shape other than the window's is drawn into targets of the
        // picture's size, so that the frame is the picture and its effects
        // lie on it (rawSnapshot).
        {
            const F32 window_aspect = (F32)gViewerWindow->getWindowWidthRaw() / (F32)llmax(gViewerWindow->getWindowHeightRaw(), 1);
            const F32 aspect = chosenAspect();
            sShapedRender = aspect > 0.f && std::fabs(aspect - window_aspect) > 0.002f;
        }
        U64 peak_footprint = 0; // before each frame and after the last, for the log
        for (S32 f = 0; f < frames && rendered; ++f)
        {
            // Stop, or Esc, on the overlay: taken from the queue here, as the
            // main loop that would handle them waits for the render
            if (sProgress && LuminousPictureView::pollStop())
            {
                LL_INFOS("LuminousHDRSnapshot") << "Render stopped after " << f << " of " << frames << " frames" << LL_ENDL;
                stopped = true;
                break;
            }
            // Before every frame, the first too: a viewer already past the
            // limit does not begin. macOS's figure unread (0), there is no
            // limit, and the log says so.
            const U64 footprint = footprintBytes();
            peak_footprint = llmax(peak_footprint, footprint);
            if (footprint == 0 && f == 0)
            {
                LL_WARNS("LuminousHDRSnapshot") << "The viewer's memory footprint could not be read: no memory limit for this render" << LL_ENDL;
            }
            const S32 pressure = memoryPressure();
            if (footprint > memory_limit || pressure >= 4)
            {
                LL_WARNS("LuminousHDRSnapshot") << "Render stopped after " << f << " of " << frames << " frames: the viewer uses "
                    << footprint / (1024 * 1024) << " MB of " << memory_limit / (1024 * 1024) << " MB allowed, macOS's memory pressure "
                    << pressure << LL_ENDL;
                too_big = true;
                break;
            }
            LuminousPhotoMode::setAccumulationFrame(frames > 1 ? f : -1);
            sAverageFrame = f;
            rendered = gViewerWindow->rawSnapshot(raw, render_w, render_h,
                                                  false /*keep_window_aspect*/, false /*is_texture*/,
                                                  false /*show_ui*/, false /*show_hud*/,
                                                  false /*do_rebuild*/, false /*no_post*/, false /*show_balance*/,
                                                  LLSnapshotModel::SNAPSHOT_TYPE_COLOR, S32_MAX);
            if (sProgress && rendered)
            {
                // how far, shown at once: the main loop waits for the render
                LuminousPictureView::setProgress((F32)(f + 1) / (F32)frames,
                                                 frames > 1 ? llformat("Rendering %d of %d", f + 1, frames) : std::string("Rendering"), true);
            }
            if (rendered && eightBit(format) && frames > 1 && !sAverage && !raw->isBufferInvalid())
            {
                const size_t n = (size_t)raw->getWidth() * raw->getHeight() * raw->getComponents();
                if (png_sum.size() != n)
                {
                    try
                    {
                        png_sum.assign(n, 0.f);
                    }
                    catch (const std::bad_alloc&)
                    {
                        put_back.now();
                        error = "not enough memory to average the frames";
                        return false;
                    }
                }
                const U8* data = raw->getData();
                for (size_t i = 0; i < n; ++i)
                {
                    png_sum[i] += data[i];
                }
                ++png_frames;
            }
            // What the frame gave back is deleted now: the main loop, which
            // would delete it, does not run until the render is done. The
            // read-back has waited for the GPU, so none of it is in use.
            // No texture unit may still think one of them bound: a name
            // deleted now can come back for a new texture next frame.
            LLImageGL::flushDeletedTextures();
            for (U32 unit = 0; unit < LL_NUM_TEXTURE_LAYERS; ++unit)
            {
                gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
            }
            gGL.getTexUnit(0)->activate();
            LLAppViewer::instance()->pingMainloopTimeout("LuminousHDRSnapshot");
        }
        peak_footprint = llmax(peak_footprint, footprintBytes());
        LL_INFOS("LuminousHDRSnapshot") << "Frames at " << render_w << "x" << render_h << ": the viewer at most "
                                        << peak_footprint / (1024 * 1024) << " MB of " << memory_limit / (1024 * 1024)
                                        << " MB allowed" << LL_ENDL;

        // What this render cost a pixel above the scene, learnt for the next
        // one: the shadow maps out, 15 % on top for what falls between the
        // readings (each before a frame, and after the last). The effects on
        // decide it -- about 140 with all of them on, fewer less -- and the
        // limit between frames still catches a change since.
        // Only from a render larger than the window, which is what the
        // estimate decides: at the window's size the shadow maps are most of
        // the cost, and what is left over reads as 70-76 bytes a pixel where
        // renders at twice the window cost 95-135.
        {
            const U64 pixels = (U64)render_w * render_h;
            if (!stopped && !too_big && rendered && footprint_now > 0 && peak_footprint > footprint_now && pixels >= 2000000
                && chosen.scale > 1.01f)
            {
                const F64 cost = ((F64)(peak_footprint - footprint_now) - (F64)render_shadow_bytes) / (F64)pixels
                                 - (eightBit(format) ? 0.0 : 20.0);
                const F32 learnt = (F32)llclamp(cost * 1.15, 70.0, 220.0);
                gSavedSettings.setF32("LuminousRenderBytesPerPixel", learnt);
                LL_INFOS("LuminousHDRSnapshot") << "This render cost about " << (S32)ll_round(cost) << " bytes a pixel above the scene; "
                                                << "the next estimate takes " << (S32)ll_round(learnt) << LL_ENDL;
            }
        }
        put_back.now();
        if (stopped)
        {
            error = "stopped";
            return false;
        }
        if (too_big)
        {
            // Full detail loads the scene finer first, and a busy region is
            // most of the memory then: say so.
            // more frames do not cost more: they reuse the same targets
            error = std::string("this region and the render together need more memory than this Mac has free.")
                + (sFullTextures ? " Try without Full detail on the Render tab."
                   : chosen.scale > 1 ? " Try the window's size on the Render tab." : "")
                + QUIT_OTHER_APPS;
            return false;
        }

        if (eightBit(format))
        {
            if (!rendered || raw->isBufferInvalid())
            {
                error = "the frame was not rendered";
                return false;
            }
            if (frames > 1 && !png_sum.empty() && png_frames > 0)
            {
                // by the frames summed, not all of them: averaging as light
                // that fails part way leaves only the later ones here
                U8* data = raw->getData();
                for (size_t i = 0; i < png_sum.size(); ++i)
                {
                    data[i] = (U8)llclamp(ll_round(png_sum[i] / (F32)png_frames), 0, 255);
                }
            }
            LL_INFOS("LuminousHDRSnapshot") << "Frame " << raw->getWidth() << "x" << raw->getHeight() << ", rendered at "
                                            << render_w << "x" << render_h << ", the picture " << picture_w << "x" << picture_h << LL_ENDL;
            if (margin_y > 0 && raw->getWidth() >= (S32)picture_w && raw->getHeight() >= (S32)picture_h)
            {
                // the picture from the frame's middle (the frame may come out
                // a pixel or two wider than asked; its pixels are the same size)
                const U32 frame_w = (U32)raw->getWidth();
                const U32 cut_x = (frame_w - picture_w) / 2;
                const U32 cut_y = ((U32)raw->getHeight() - picture_h) / 2;
                const S32 c = raw->getComponents();
                LLPointer<LLImageRaw> cut = new LLImageRaw(picture_w, picture_h, c);
                cut->enableOverSize();
                if (cut->isBufferInvalid())
                {
                    error = "not enough memory to cut the picture from its margin";
                    return false;
                }
                const U8* src = raw->getData();
                U8* dst = cut->getData();
                for (U32 y = 0; y < picture_h; ++y)
                {
                    memcpy(dst + (size_t)y * picture_w * c, src + ((size_t)(y + cut_y) * frame_w + cut_x) * c,
                           (size_t)picture_w * c);
                }
                raw = cut;
            }
            if ((raw->getWidth() != (S32)out_w || raw->getHeight() != (S32)out_h) && !raw->scale(out_w, out_h))
            {
                error = "not enough memory to scale the picture";
                return false;
            }
            if (sKeep)
            {
                *sKeep = raw;
                LL_INFOS("LuminousHDRSnapshot") << "Picture " << out_w << "x" << out_h << ", " << frames << " frames" << LL_ENDL;
                return true;
            }
            if (sShow)
            {
                *sShow = raw; // the picture itself: nothing changes it after this
            }
            if (sProgress)
            {
                LuminousPictureView::setProgress(1.f, "Saving...", true);
            }
            LLPointer<LLImageFormatted> file;
            if (format == JPEG8)
            {
                // Always quality 95: it looks as the picture does, and 100 is
                // far larger for nothing seen.
                file = new LLImageJPEG(95);
            }
            else if (format == BMP8)
            {
                file = new LLImageBMP;
            }
            else
            {
                file = new LLImagePNG;
            }
            file->enableOverSize();
            LLTimer timer;
            if (!file->encode(raw, 0.f) || !file->save(path))
            {
                error = "could not write " + path;
                return false;
            }
            LL_INFOS("LuminousHDRSnapshot") << (format == JPEG8 ? "JPEG " : format == BMP8 ? "BMP " : "PNG ")
                                            << out_w << "x" << out_h << ", " << frames << " frames, written in "
                                            << timer.getElapsedTimeF32().value() << " s: " << path << LL_ENDL;
            return true;
        }

        if (!sCaptureError.empty())
        {
            error = sCaptureError;
            return false;
        }
        if (!rendered || sFrame.rgb.empty())
        {
            error = "the frame was not rendered";
            return false;
        }
        // The frame read must be the one asked for: one rendered in tiles,
        // or at another size, would be resampled without a word.
        if (margin_y > 0 ? (sFrame.width < picture_w || sFrame.height < picture_h || sFrame.width > render_w + 64)
                         : (sFrame.width != render_w || sFrame.height != render_h))
        {
            const LLRect window = gViewerWindow->getWorldViewRectRaw();
            error = render_w > (U32)window.getWidth() || render_h > (U32)window.getHeight()
                ? llformat("it is too large to render in one piece (%u x %u)", render_w, render_h)
                : llformat("the frame came out %u x %u, not %u x %u", sFrame.width, sFrame.height, render_w, render_h);
            return false;
        }

        if (sFrameCount > 1)
        {
            const F32 scale = 1.f / (F32)sFrameCount;
            for (F32& v : sFrame.rgb)
            {
                v *= scale;
            }
        }
        std::vector<F32> picture;
        try
        {
            if (margin_y > 0)
            {
                // The picture from the frame's middle, moved down in place:
                // each row lands at or below where it was read, so no second
                // buffer the picture's size (637 MB at twice a 5K window).
                const U32 cut_x = (sFrame.width - picture_w) / 2;
                const U32 cut_y = (sFrame.height - picture_h) / 2;
                for (U32 y = 0; y < picture_h; ++y)
                {
                    memmove(&sFrame.rgb[(size_t)y * picture_w * 3], &sFrame.rgb[((size_t)(y + cut_y) * sFrame.width + cut_x) * 3],
                            (size_t)picture_w * 3 * sizeof(F32));
                }
                sFrame.rgb.resize((size_t)picture_w * picture_h * 3);
                sFrame.width = picture_w;
                sFrame.height = picture_h;
            }
            if (sFrame.width == out_w && sFrame.height == out_h)
            {
                // Nothing to reduce: not copied, only turned top row first in
                // place, as reduce() gives it; GL reads the bottom row first.
                picture = std::move(sFrame.rgb);
                const size_t row = (size_t)out_w * 3;
                F32* rows = picture.data();
                for (U32 y = 0; y < out_h / 2; ++y)
                {
                    std::swap_ranges(rows + y * row, rows + (y + 1) * row, rows + (out_h - 1 - y) * row);
                }
            }
            else
            {
                reduce(sFrame, picture_w, picture_h, out_w, out_h, picture);
            }
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory";
            return false;
        }
        sFrame = Frame();

        if (sShow)
        {
            // An eight-bit copy to show, bottom row first as images are: the
            // TIFF's values are the finished picture's; the EXR's are light,
            // shown at the render's exposure, clipped -- without the Look's
            // tone, which an EXR leaves to its editor.
            LLPointer<LLImageRaw> shown = new LLImageRaw(out_w, out_h, 3);
            shown->enableOverSize();
            if (!shown->isBufferInvalid())
            {
                U8* data = shown->getData();
                for (U32 y = 0; y < out_h; ++y)
                {
                    const F32* src = &picture[(size_t)y * out_w * 3];
                    U8* dst = data + (size_t)(out_h - 1 - y) * out_w * 3;
                    for (U32 i = 0; i < out_w * 3; ++i)
                    {
                        F32 v = src[i];
                        if (format == EXR)
                        {
                            v = llclamp(v * sExposure, 0.f, 1.f);
                            v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
                        }
                        dst[i] = (U8)ll_round(llclamp(v, 0.f, 1.f) * 255.f);
                    }
                }
                *sShow = shown;
            }
        }

        if (sProgress)
        {
            LuminousPictureView::setProgress(1.f, "Saving...", true);
        }
        const bool written = format == EXR ? writeEXR(path, out_w, out_h, picture, sExposure)
                                           : writeTIFF16(path, out_w, out_h, picture);
        if (!written)
        {
            error = "could not write " + path;
            return false;
        }
        LL_INFOS("LuminousHDRSnapshot") << (format == EXR ? "EXR" : "16-bit TIFF") << " " << out_w << "x" << out_h
                                        << " from " << frames << " frames of " << render_w << "x" << render_h << ", exposure " << sExposure
                                        << ": " << path << LL_ENDL;
        return true;
    }

    bool wantsFullTextures()
    {
        return sFullTextures;
    }

    F32 fullDetailAreaScale()
    {
        if (!sFullTextures)
        {
            return 1.f;
        }
        // The picture's pixels per pixel of the window, the margin included,
        // and that area four times over: a level of detail finer than the
        // picture shows, for its shifted frames and surfaces seen at a slant.
        // A texture large on screen still loads at full size.
        static LLCachedControl<U32> scale_setting(gSavedSettings, "LuminousSnapshotScale", 1);
        static LLCachedControl<bool> margin(gSavedSettings, "LuminousSnapshotMargin", true);
        const F32 side = (F32)llclamp((U32)scale_setting(), 1U, 2U) * (margin() && LuminousPhotoMode::isOn() ? 1.2f : 1.f);
        return side * side * 4.f;
    }

    void init()
    {
        // Renders left in the temporary folder by a session that ended
        // without closing them (a crash): theirs only, by name.
        const std::string temp = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, "");
        const bool ours = !temp.empty() && temp.find("Luminous") != std::string::npos && LLFile::isdir(temp);
        for (const std::string& name : ours ? gDirUtilp->getFilesInDir(temp) : std::vector<std::string>())
        {
            if (name.rfind("Luminous_", 0) == 0)
            {
                LLFile::remove(gDirUtilp->add(temp, name));
            }
        }
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.SaveHDRSnapshot",
            [](LLUICtrl*, const LLSD& param) { save(EXR, param); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.Save16BitSnapshot",
            [](LLUICtrl*, const LLSD& param) { save(TIFF16, param); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.SaveLargeSnapshot",
            [](LLUICtrl*, const LLSD& param) { save(PNG8, param); });
        // The Luminous window's one Save button, in the format chosen beside
        // it; with a path (LEAP), only saved there.
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.SaveSnapshot",
            [](LLUICtrl*, const LLSD& param)
            {
                if (!param.asString().empty())
                {
                    static LLCachedControl<U32> format(gSavedSettings, "LuminousSnapshotFormat", 0);
                    const EFormat formats[] = { PNG8, TIFF16, EXR, JPEG8, BMP8 };
                    save(formats[llmin((U32)format(), 4U)], param);
                    return;
                }
                renderImage();
            });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Luminous.PreviewImage",
            [](LLUICtrl*, const LLSD&) { renderPreview(); });
    }

    F32 chosenAspect()
    {
        // landscape as listed; Portrait turns them upright
        static const F32 ASPECTS[] = { 0.f, 1.f, 5.f / 4.f, 4.f / 3.f, 3.f / 2.f, 16.f / 9.f, 21.f / 9.f };
        static LLCachedControl<S32> aspect(gSavedSettings, "LuminousSnapshotAspect", 0);
        static LLCachedControl<bool> portrait(gSavedSettings, "LuminousSnapshotPortrait", false);
        const F32 a = ASPECTS[llclamp((S32)aspect(), 0, (S32)LL_ARRAY_SIZE(ASPECTS) - 1)];
        return a > 0.f && portrait() ? 1.f / a : a;
    }

    void pictureSize(F32 scale, U32& width, U32& height)
    {
        const U32 most = (U32)gGLManager.mGLMaxTextureSize;
        const F32 window_w = (F32)gViewerWindow->getWindowWidthRaw() * scale;
        const F32 window_h = (F32)gViewerWindow->getWindowHeightRaw() * scale;
        F32 w = window_w;
        F32 h = window_h;
        const F32 aspect = chosenAspect();
        if (aspect > 0.f && window_h > 0.f)
        {
            if (aspect < window_w / window_h)
            {
                w = window_h * aspect; // narrower: the height kept, the sides cut
            }
            else
            {
                h = window_w / aspect; // wider: the width kept, top and bottom cut
            }
        }
        width = llclamp((U32)ll_round(w), 16U, most);
        height = llclamp((U32)ll_round(h), 16U, most);
    }

    bool shapedRender()
    {
        return sShapedRender;
    }

    F32 pictureAspect()
    {
        // as takeAndSave sizes the picture
        static LLCachedControl<U32> scale_setting(gSavedSettings, "LuminousSnapshotScale", 1);
        U32 w, h;
        pictureSize(sPreview ? 1.f : (F32)llclamp((U32)scale_setting(), 1U, 2U), w, h);
        return (F32)w / (F32)llmax(h, 1U);
    }

    bool previewing()
    {
        return sPreview;
    }

    void renderPreview()
    {
        if (sWaiting)
        {
            report("A snapshot is already being taken.");
            return;
        }
        sPreview = true;
        // JPEG: quick to write, and only shown, never kept
        saveWhenSharp(JPEG8, gDirUtilp->getExpandedFilename(LL_PATH_TEMP, defaultName(JPEG8)), TEMPORARY);
    }

    void endPreview()
    {
        if (sPreviewHold)
        {
            sPreviewHold = false;
            setAnimationTime(sPreviewHeldFactor);
            LLEyeMotion::sHoldGaze = false;
        }
    }

    void renderImage()
    {
        static LLCachedControl<U32> format(gSavedSettings, "LuminousSnapshotFormat", 0);
        static LLCachedControl<bool> to_disk(gSavedSettings, "LuminousRenderToDisk", false);
        // The setting's values, which are not in the menu's order.
        const EFormat formats[] = { PNG8, TIFF16, EXR, JPEG8, BMP8 };
        const EFormat chosen = formats[llmin((U32)format(), 4U)];
        if (to_disk)
        {
            save(chosen, LLSD(), TO_DISK); // into the folder, asked for the first time
            return;
        }
        // Kept only in a temporary file, shown when done, until downloaded
        // or sent on.
        saveWhenSharp(chosen, gDirUtilp->getExpandedFilename(LL_PATH_TEMP, defaultName(chosen)), TEMPORARY);
    }
}
