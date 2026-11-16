/**
 * @file luminousobjectmotion.h
 * @brief MetalFX temporal: the motion of what moves by itself, for the scaler's motion texture and the settles.
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

#ifndef LUMINOUS_OBJECT_MOTION_H
#define LUMINOUS_OBJECT_MOTION_H

class LLDrawable;
class LLMatrix4;
class LLViewerTextureAnim;
class LLVOAvatar;

namespace LL::RHI
{
class Texture;
}

// MetalFX temporal's object motion, on Metal only. The motion texture the scaler gets is drawn at the present from the
// frame's depth and the camera's matrices: the camera's motion. This pass draws, once the main view's depth is final,
// what moved by itself since the last frame -- opaque, cut out, and alpha -- into a motion target of the scaler's input
// size, each fragment kept only where it lies on the surface the frame shows: the rigged meshes and animesh whose
// joints moved, skinned with this frame's palette and the last frame's; the system avatar's parts that moved, skinned
// with this frame's joint matrices and the last frame's (its eyeballs through their world matrices); and what is not
// rigged whose model matrix moved (attachments, vehicles, moving and turning objects), through this frame's model
// matrix and the last frame's; the quad of each impostored avatar whose render position moved, as the avatar pool draws
// it, moved by that change; and what is opaque and not rigged whose texture animation moved smoothly in this frame,
// with the texture's own motion added to its model matrix's (or the camera's). The present takes a texel's motion from
// it where it was drawn into, and the camera's elsewhere; the settles of depth of field, the glow and Enhance read
// their last average where it says the surface was (luminousmetaltone.cpp). LuminousMetalFXObjectMotion switches it
// off. Its target's blue channel is a reactive output where a draw whose surface changes in a way its motion does not
// give is the surface the frame shows: 1 for a flexible prim whose shape was rebuilt in this frame, drawn even where
// its model matrix did not move, and a texture animation that stepped in this frame by a frame, or smoothly on a
// cut-out or alpha draw (a rigged one by a frame only, where its palette moved), and an impostor whose picture was made
// again in this frame, drawn even where its avatar did not move; 0 elsewhere. Where it drew such draws, a stage of
// MetalFX's reactive mask is drawn from it after the pass (LuminousMetalFXObjectReactive its strength; 0 none), faded
// with distance as the alpha's.
namespace LuminousObjectMotion
{
/// Main thread, in the main view's deferred lighting after the post-deferred geometry, no target bound: draws this
/// frame's object motion where LuminousMetal::objectMotionWanted says it will be used, LuminousMetalFXObjectMotion is
/// on and a motion program has compiled (the rigged draws with the rigged one, the system avatar's skinned parts with
/// the avatar one, the smooth texture animation steps of opaque draws with the texture one, the others and the
/// impostors' quads with the plain one), and tells the present and the settles (LuminousMetal::objectMotionDrawn) when
/// anything was drawn; where draws marked reactive were drawn, a stage of the reactive mask follows
/// (LuminousMetal::temporalObjectReactive).
/// Where it does not draw in the main view of frames drawn for the scaler, the log gets one line naming why once the
/// reason has held for a moment (again only when it changes), and one when it draws again. Nothing on OpenGL.
void draw();

/// The target the pass draws into (RGBA16F, the scaler's input size: the motion in red and green, the reactive output
/// in blue), or null before the first pass.
LL::RHI::Texture* texture();

/// Main thread, at the start of display(), before the geometry is updated and the impostors are made: the flexible
/// prims reshaped and the avatars whose impostor was made in the frame before are forgotten.
void beginFrame();

/// Main thread, as a flexible prim's positions are rebuilt (LLVolumeImplFlexible::doUpdateGeometry): its drawable is
/// kept for this frame's pass, which draws its faces as reactive. Only while LuminousMetal::objectMotionOn().
void reshaped(LLDrawable* drawable);

/// Main thread, as an avatar's impostor is made again (LLPipeline::generateImpostor, not for a preview or a profile):
/// the avatar is kept, by its address and id, for this frame's pass, which draws its impostor's quad as reactive. Only
/// while LuminousMetal::objectMotionOn().
void impostorRefreshed(const LLVOAvatar* avatar);

/// Main thread, at the start of each idle's object update (LLViewerObjectList::update), before the texture animations
/// step (LLViewerTextureAnim::updateClass), also where they do not: the steps of the last update are forgotten.
void beginTextureSteps();

/// Main thread, in LLVOVolume::animateTextures after the animation stepped, its counter at frame_before before the
/// step: whether the step moved the texture smoothly -- the animation SMOOTH, its counter moved by no more than twice
/// its rate times the frame's time (apart from whole repeats for a TRANSLATE without a grid, whose counter is its
/// offset in s, whole rows for a grid's TRANSLATE of one row, and whole turns for a ROTATE), and for a grid's TRANSLATE
/// of more rows in the same row -- or by a frame (any other step: a frame of a flipbook, a loop's wrap by other than
/// those, a grid's next row, a start).
bool textureStepSmooth(const LLViewerTextureAnim& anim, F32 frame_before);

/// Main thread, in LLVOVolume::animateTextures just before a face's texture matrix is set for the step: its value
/// before the step is kept, by the matrix's address, with whether the step was smooth (textureStepSmooth), for the pass
/// of the frame drawn after it; one made in this step (made) has no last frame and gets no draw. Only while
/// LuminousMetal::objectMotionOn().
void textureStepped(const LLMatrix4* matrix, bool made, bool smooth);

/// The target let go and the last frame's model matrices, avatar roots and render positions, system avatar joint
/// matrices, reshaped flexible prims, refreshed impostors and texture animations' steps forgotten, with MetalFX
/// temporal's scaler and with the pipeline's screen-sized targets; made again when next drawn.
void release();
}

#endif // LUMINOUS_OBJECT_MOTION_H
