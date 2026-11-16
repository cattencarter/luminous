/**
 * @file luminousobjectmotion.cpp
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

#include "llviewerprecompiledheaders.h"

#include "luminousobjectmotion.h"

#include "luminousmetal.h"
#include "luminousmetalparams.h"

#include "llappviewer.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "lldrawpoolwater.h"
#include "llenvironment.h"
#include "llface.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmodel.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjoint.h"
#include "llviewerjointmesh.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertextureanim.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "pipeline.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>

// The pass, in the main view's deferred lighting of a frame drawn for MetalFX's temporal scaler, once the frame's depth
// is final (the opaque and cut-out geometry, the post-deferred geometry, worn alpha's depth, the water):
// - its own target: RGBA16F, the motion in texels in red and green, cleared to LUMINOUS_TONE_OBJECT_MOTION_NONE, the
//   reactive output in blue, cleared to 0, alpha 0; and its own depth (memoryless on Metal), cleared to 1;
// - the rigged draws of the main view's cull (the types below, opaque and cut out, and the worn alpha groups), each
//   only where its (avatar, skin) palette was built in the last frame too and changed since, and its avatar did not
//   jump (avatarJumped); both palettes uploaded once per run of draws with the same avatar and skin;
// - the draws of the same types that are not rigged, and of the alpha groups whose alpha writes its depth (the
//   pre-water pool's; every group with depth of field), each only where its model matrix moved since the last frame:
//   attachments that follow a joint, vehicles, scripted and physical movers, the parts of a linkset that moves. Static
//   objects are drawn through their region's own matrix, and those draws are passed over before the table. The model
//   matrices are kept from frame to frame in a table by their address; an entry not seen in the last frame, or one
//   whose place jumped farther than a fast vehicle goes in the time since it (modelJump), is a first sight and not
//   drawn. A flexible prim gives its model matrix's motion; its own bending is not in it (but see the reactive draws
//   below), nor is a texture's own animation (but see the texture animations below);
// - reactive draws, where LuminousMetalFXObjectReactive is above 0 and this frame has a reactive mask: the faces of the
//   flexible prims whose positions were rebuilt in this frame (reshaped), as ranges of their vertex buffers. A draw of
//   the types and alpha groups above that is not rigged and overlaps one of them is drawn with luminous_reactive 1, and
//   also where its model matrix did not move, is a first sight or is a region's: then with this frame's matrix as the
//   last frame's too, which gives the camera's motion. Where its fragment is kept, blue is 1; after the pass a stage
//   of the reactive mask is drawn from it (LuminousMetal::temporalObjectReactive), faded with distance as the alpha's.
//   A sibling batched into the same draw is marked with it. Also reactive, with 1: the quads of the impostors made
//   again in this frame (below) and the texture animations' frame steps (below);
// - texture animations: the face texture matrices LLVOVolume::animateTextures set again in this frame's object update,
//   each kept with its value before the step by its address (textureStepped; forgotten at the start of each update,
//   beginTextureSteps), and whether the step was smooth (the animation SMOOTH, its counter moved by no more than twice
//   its rate in the frame's time, apart from whole repeats for a TRANSLATE without a grid, whole rows for a grid's
//   TRANSLATE of one row and whole turns for a ROTATE; a grid's TRANSLATE of more rows in its row) or by a frame (any
//   other: a flipbook's frame, a loop's wrap by other than those, a grid's next row). A draw of the types and alpha
//   groups above that is not rigged and whose texture matrix stepped is drawn also where its model matrix did not
//   move, is a first sight or is a region's (then with this frame's matrix as the last frame's too, which gives the
//   camera's motion; a moving one with its entry in the table): a smooth step of an opaque type with the texture
//   variant, which adds the texture's own shift to that motion, reactive where the draw overlaps a reshaped flexible
//   prim; a frame step, a smooth step of a cut-out type or an alpha group (whose fragments are kept by depth alone,
//   also where their holes or their blending show what lies behind), or a smooth one without the texture variant, with
//   the plain program and luminous_reactive 1, where reactive draws are taken. Each draw is drawn once, with one
//   program. A matrix made in the step has no last frame and adds nothing. A rigged draw drawn for its palette is
//   reactive where its texture matrix stepped by a frame (mesh eyes); a rigged texture's smooth motion is not in it;
// - the system avatar's parts (head, upper and lower body, eyelashes, hair, skirt, eyeballs) where the avatar pool
//   draws them in this view (an avatar it draws in full: not impostored, and its appearance normal or its impostor to
//   be made again; a part whose baked texture is not hidden by an alpha layer; the level of detail it picks), each only
//   where it moved since the last frame and its avatar did not jump: the skinned parts with their joint matrices of
//   this frame and the last (built here each pass, to the agent frame, and kept by the part's address with its
//   avatar's id from frame to frame while the pass runs), the eyeballs through their world matrix in the table of
//   model matrices. The wind's ripple on system clothing (RenderAvatarCloth) is not in it;
// - the impostored avatars the avatar pool draws as their impostor in this view (not the agent's own, nor an animesh it
//   wears): one quad each, with the corners renderImpostor gives it in this frame (its render position plus the
//   impostor's offset, facing the camera, the impostor's size), where its render position moved since the last frame
//   by no more than modelJump, or its impostor was made again in this frame (LLPipeline::generateImpostor ->
//   impostorRefreshed). The corners are in the agent frame, as renderImpostor's; this frame's model matrix is the
//   identity and the last frame's a translation by the render position's change, so both frames have the same quad
//   but for that move. One made again is drawn reactive (luminous_reactive 1), with no move where it did not move or
//   is a first sight (not seen in the frame before, or its render position moved farther than modelJump). The quad
//   covers its whole rectangle, and a fragment is kept where the frame's depth is the quad's: where the deferred
//   impostor program wrote the picture (alpha 0.01 or more);
// - an avatar jumped where its root joint (what its palettes and joint matrices hang from) was not seen in the frame
//   before or moved farther since than modelJump: then none of its skins and parts is drawn;
// - luminousMotionV/F.glsl's four variants (rigged: two palettes; the system avatar: two sets of joint matrices; not
//   rigged and the impostors' quads: two model matrices; texture animations: two model matrices and two texture
//   matrices): a fragment is kept where its distance lies within a small tolerance of the frame's depth at the texel,
//   its depth is that difference over the tolerance, and the test LESS keeps the one closest to the surface the frame
//   shows;
// - no blending, no culling (the depth comparison leaves out back faces that are not the surface shown, and
//   double-sided materials are drawn unculled by the deferred pools), no texture but the frame's depth.
// The pass is begun only at its first draw: with nothing moving it costs no pass, and the present and the settles take
// the camera's motion everywhere.
namespace LuminousObjectMotion
{
    namespace
    {
        LLRenderTarget sTarget;
        bool sAllocationRefused = false; // said once

        // The types the main view's deferred and post-deferred pools draw opaque or cut out, each once; the rigged form
        // of each is the type after it (LLRenderPass). Not here: SHINY (registered only without shaders, or for the
        // HUD), POST_BUMP and the glow passes (drawn over faces that are in one of these already), the blended material
        // types (drawn in the alpha groups), INVISIBLE and ALPHA_INVISIBLE (water exclusion surfaces and the like,
        // never drawn in colour).
        constexpr U32 TYPES[] = {
            LLRenderPass::PASS_SIMPLE,
            LLRenderPass::PASS_FULLBRIGHT,
            LLRenderPass::PASS_FULLBRIGHT_SHINY,
            LLRenderPass::PASS_BUMP,
            LLRenderPass::PASS_MATERIAL,
            LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
            LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
            LLRenderPass::PASS_SPECMAP,
            LLRenderPass::PASS_SPECMAP_MASK,
            LLRenderPass::PASS_SPECMAP_EMISSIVE,
            LLRenderPass::PASS_NORMMAP,
            LLRenderPass::PASS_NORMMAP_MASK,
            LLRenderPass::PASS_NORMMAP_EMISSIVE,
            LLRenderPass::PASS_NORMSPEC,
            LLRenderPass::PASS_NORMSPEC_MASK,
            LLRenderPass::PASS_NORMSPEC_EMISSIVE,
            LLRenderPass::PASS_ALPHA_MASK,
            LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK,
            LLRenderPass::PASS_GLTF_PBR,
            LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK,
        };

        // Whether a type of TYPES is cut out (alpha masked): what lies behind it shows through its holes.
        bool cutOut(U32 type)
        {
            switch (type)
            {
                case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:
                case LLRenderPass::PASS_SPECMAP_MASK:
                case LLRenderPass::PASS_NORMMAP_MASK:
                case LLRenderPass::PASS_NORMSPEC_MASK:
                case LLRenderPass::PASS_ALPHA_MASK:
                case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
                case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK:
                    return true;
                default:
                    return false;
            }
        }

        // A model matrix, or an avatar's root joint, that moved farther since the last frame than MODEL_JUMP plus
        // MODEL_SPEED times the time since it (MODEL_TIME_MOST at most) is a first sight: a teleport (within the
        // region too), a sit far away, a seat moved by llSetRegionPos, a region's 256 m shift, an address used again.
        constexpr F32 MODEL_JUMP = 4.f;        // metres
        constexpr F32 MODEL_SPEED = 150.f;     // metres a second
        constexpr F32 MODEL_TIME_MOST = 0.5f;  // seconds
        constexpr U32 MODEL_KEEP = 8;   // frames an entry of the table is kept while unseen
        constexpr U32 MODEL_SWEEP = 64; // frames between sweeps of the table

        // A model matrix of draws that are not rigged (a drawable's world matrix, or a linkset root's that its parts
        // share): this frame's and the last frame's, kept by its address from frame to frame while the pass runs.
        struct Model
        {
            LLMatrix4 now;
            LLMatrix4 last;
            U32 frame = 0;      // gFrameCount when it was last seen
            bool moved = false; // seen in the frame before as well, and changed since by no more than modelJump
        };
        std::unordered_map<const LLMatrix4*, Model> sModels;
        U32 sModelsSwept = 0; // gFrameCount at the last sweep
        F32 sModelJump = MODEL_JUMP; // this pass's limit in metres (modelJump), set at its start
        std::vector<const LLMatrix4*> sRegionMatrices; // this pass's: every region's own matrix, sorted

        // An avatar's root joint, which its rigged palettes and its system parts' joint matrices hang from: its place
        // in the agent frame when the pass last looked at the avatar, kept by the avatar's address with its id from
        // frame to frame while the pass runs (avatarJumped); with its render position, which its impostor is drawn at.
        struct AvatarRoot
        {
            LLUUID avatar;
            LLVector3 place;
            LLVector3 render;     // LLVOAvatar::getRenderPosition when it was last seen
            LLVector3 lastRender; // in the frame before, where follows
            U32 frame = 0;        // gFrameCount when it was last seen
            bool follows = false; // seen in the frame before as well, for this avatar
            bool jumped = false;  // not seen in the frame before for this avatar, or moved farther than sModelJump
        };
        std::unordered_map<const LLVOAvatar*, AvatarRoot> sAvatarRoots;

        // The avatars whose impostor was made again since the start of this frame's display() (impostorRefreshed), by
        // their address and id.
        std::vector<std::pair<const LLVOAvatar*, LLUUID>> sRefreshed;

        // A system avatar's skinned part: its joint matrices to the agent frame
        // (LLViewerJointMesh::luminousJointMatrices) of this frame and the last, kept by the part's address from
        // frame to frame while the pass runs; with the avatar's id, so that an address taken by another avatar is a
        // first sight.
        constexpr U32 JOINT_FLOATS = LLViewerJointMesh::LUMINOUS_JOINT_FLOATS;
        struct AvatarPalette
        {
            LLUUID avatar;
            U32 frame = 0;      // gFrameCount when it was last built
            U32 now = 0;        // which of matrices is this frame's
            bool moved = false; // built in the frame before as well, for the same avatar, which did not jump since, and
                                // changed since
            F32 matrices[2][JOINT_FLOATS];
        };
        std::unordered_map<const LLViewerJointMesh*, AvatarPalette> sAvatarPalettes;

        // A system avatar's part the avatar pool draws in this view, found once a pass (collectAvatarParts).
        struct AvatarPart
        {
            LLViewerJointMesh* mesh = nullptr;
            const LLVOAvatar* avatar = nullptr;
            bool jumped = false; // its avatar's (avatarJumped)
            LLViewerJointMesh::LuminousMotionDraw draw;
        };
        std::vector<AvatarPart> sAvatarParts;
        std::vector<LLViewerJoint*> sAvatarJoints; // one avatar's parts, as it picks them

        // The flexible prims whose positions were rebuilt since the start of this frame's display() (reshaped), and
        // this pass's ranges of their faces' vertices, sorted by vertex buffer (collectReactiveRanges).
        std::vector<LLPointer<LLDrawable>> sReshaped;
        struct ReactiveRange
        {
            const LLVertexBuffer* buffer = nullptr;
            U32 start = 0; // the first vertex and the last
            U32 end = 0;
        };
        std::vector<ReactiveRange> sReactiveRanges;

        // A smooth step's counter may have moved by this many times the animation's rate times the frame's time.
        constexpr F32 TEXTURE_STEP_MOST = 2.f;

        // The face texture matrices set again in this frame's object update (textureStepped): each one's address, its
        // value before the step, whether the step was smooth, and whether the matrix was made in it. Appended in the
        // order of the steps, into a vector that keeps its memory from update to update; looked up by address through
        // sTextureStepOrder, which sortTextureSteps makes once after the update: one entry per matrix, the one of its
        // first step, not smooth where it stepped again in the same update.
        struct TextureStep
        {
            const LLMatrix4* matrix = nullptr;
            LLMatrix4 last;
            bool smooth = false;
            bool made = false; // no last frame
        };
        std::vector<TextureStep> sTextureSteps;
        std::vector<U32> sTextureStepOrder; // indices into sTextureSteps, by address
        bool sTextureStepsSorted = true;    // sTextureStepOrder holds every step appended
        struct TextureStepCounts
        {
            U32 smooth = 0; // matrices that stepped smoothly
            U32 frame = 0;  // by a frame
            U32 made = 0;   // made in the step
        };
        TextureStepCounts sTextureStepCounts; // of sTextureStepOrder's entries

        bool addressBefore(const LLMatrix4* a, const LLMatrix4* b)
        {
            return std::less<const LLMatrix4*>()(a, b);
        }

        // sTextureStepOrder made from the steps appended since the last update began, and their counts, where steps
        // were appended since it was last made.
        void sortTextureSteps()
        {
            if (sTextureStepsSorted)
            {
                return;
            }
            sTextureStepsSorted = true;
            sTextureStepOrder.resize(sTextureSteps.size());
            for (U32 i = 0; i < (U32)sTextureStepOrder.size(); ++i)
            {
                sTextureStepOrder[i] = i;
            }
            // by address, and a matrix's steps in their order
            std::sort(sTextureStepOrder.begin(), sTextureStepOrder.end(), [](U32 a, U32 b)
                      {
                          const LLMatrix4* first = sTextureSteps[a].matrix;
                          const LLMatrix4* second = sTextureSteps[b].matrix;
                          return addressBefore(first, second) || (first == second && a < b);
                      });
            size_t kept = 0;
            for (size_t i = 0; i < sTextureStepOrder.size(); ++i)
            {
                const U32 index = sTextureStepOrder[i];
                if (kept > 0 && sTextureSteps[sTextureStepOrder[kept - 1]].matrix == sTextureSteps[index].matrix)
                {
                    sTextureSteps[sTextureStepOrder[kept - 1]].smooth = false; // stepped again in the same update
                }
                else
                {
                    sTextureStepOrder[kept++] = index;
                }
            }
            sTextureStepOrder.resize(kept);
            sTextureStepCounts = TextureStepCounts();
            for (U32 index : sTextureStepOrder)
            {
                const TextureStep& step = sTextureSteps[index];
                if (step.made)
                {
                    ++sTextureStepCounts.made;
                }
                else if (step.smooth)
                {
                    ++sTextureStepCounts.smooth;
                }
                else
                {
                    ++sTextureStepCounts.frame;
                }
            }
        }

        const LLStaticHashedString& lastPaletteName()
        {
            static const LLStaticHashedString name("lastMatrixPalette");
            return name;
        }

        const LLStaticHashedString& modelName()
        {
            static const LLStaticHashedString name("luminous_model");
            return name;
        }

        const LLStaticHashedString& lastModelName()
        {
            static const LLStaticHashedString name("luminous_last_model");
            return name;
        }

        const LLStaticHashedString& viewProjectionName()
        {
            static const LLStaticHashedString name("luminous_view_proj");
            return name;
        }

        const LLStaticHashedString& lastViewProjectionName()
        {
            static const LLStaticHashedString name("luminous_last_view_proj");
            return name;
        }

        const LLStaticHashedString& depthTermsName()
        {
            static const LLStaticHashedString name("luminous_depth_terms");
            return name;
        }

        const LLStaticHashedString& reactiveName()
        {
            static const LLStaticHashedString name("luminous_reactive");
            return name;
        }

        // luminous_reactive's values, written to the target's blue where a draw's fragment is kept: none; a mark.
        constexpr F32 REACTIVE_NONE = 0.f;
        constexpr F32 REACTIVE_MARK = 1.f;

        const LLStaticHashedString& textureNowName()
        {
            static const LLStaticHashedString name("luminous_texture_now");
            return name;
        }

        const LLStaticHashedString& textureLastName()
        {
            static const LLStaticHashedString name("luminous_texture_last");
            return name;
        }

        struct Counts
        {
            U32 draws = 0;        // rigged draws drawn
            U32 skins = 0;        // runs of (avatar, skin) whose palettes were uploaded
            U32 still = 0;        // runs whose palette did not change since the last frame
            U32 unseen = 0;       // runs without a palette of the last frame, or of an avatar that jumped
            U32 modelDraws = 0;   // draws through a model matrix that moved
            U32 modelRuns = 0;    // runs of draws with the same model matrix whose matrices were uploaded
            U32 models = 0;       // model matrices seen this frame that moved since the last
            U32 modelsStill = 0;  // that did not
            U32 modelsUnseen = 0; // without a last frame: not seen in it, moved farther than modelJump, or the eyeball
                                  // of an avatar that jumped
            U32 avatarDraws = 0;  // system avatar parts drawn with their joint matrices
            U32 avatarParts = 0;  // skinned system avatar parts that moved since the last frame
            U32 avatarStill = 0;  // that did not
            U32 avatarUnseen = 0; // without a last frame, or of an avatar that jumped
            U32 reactiveDraws = 0;  // draws marked reactive (overlapping a reshaped flexible prim's faces, or the quad
                                    // of an impostor made again)
            U32 reactiveRanges = 0; // the ranges of reshaped flexible prims' faces this pass looked for
            U32 impostorDraws = 0;     // impostors' quads drawn
            U32 impostorsReactive = 0; // of them, of an impostor made again in this frame (also in reactiveDraws)
            U32 impostorsMoved = 0;    // impostors the avatar pool draws in this view whose render position moved
            U32 impostorsStill = 0;    // whose did not
            U32 impostorsUnseen = 0;   // not seen in the frame before, or moved farther than modelJump
            U32 textureSmoothDraws = 0; // draws with the texture program: a smooth texture animation step's own motion
            U32 textureFrameDraws = 0;  // draws marked reactive for a texture animation's frame step (rigged ones
                                        // too), or for a smooth step of a draw that is not rigged where it is cut out
                                        // or alpha or the texture program is not ready (also in reactiveDraws)
            U32 textureSmooth = 0;      // texture matrices that stepped smoothly in this frame's object update
            U32 textureFrame = 0;       // that stepped by a frame
            U32 textureMade = 0;        // made in the step: no last frame
        };

        // While the Metal pass timer runs (LuminousMetalPassTimes), the pass's own time on the main thread and what it
        // drew, for one line in the log once the timer is done.
        struct Timing
        {
            U32 passes = 0;
            U64 total = 0; // microseconds
            U64 most = 0;
            U64 draws = 0;
            U64 skins = 0;
            U64 modelDraws = 0;
            U64 models = 0;
            U64 modelsSeen = 0;
            U64 avatarDraws = 0;
            U64 avatarParts = 0;
            U64 reactiveDraws = 0;
            U64 impostorDraws = 0;
            U64 textureSmoothDraws = 0;
            U64 textureFrameDraws = 0;
        };
        Timing sTiming;

        // The run of rigged draws with the same (avatar, skin): whether its palette moved, and whether the program
        // holds it.
        struct Skin
        {
            const LLVOAvatar* avatar = nullptr;
            U64 hash = 0;
            bool moved = false;
            bool uploaded = false;
        };

        // The run of draws with the same model matrix: its entry (null for none, or a region's), and whether the plain
        // program and the texture program hold it.
        struct ModelRun
        {
            const LLMatrix4* matrix = nullptr;
            const Model* model = nullptr;
            bool uploaded = false;
            bool textureUploaded = false;
        };

        // The pass: the target bound and cleared at the first draw; each program bound when its draws come, with this
        // frame's uniforms at its first use; all of it let go again at the end. luminous_reactive stays on a program
        // from draw to draw and frame to frame: it is set to REACTIVE_NONE at the program's first use in the pass, and
        // after that only where a draw changes it.
        class Pass
        {
        public:
            explicit Pass(const LuminousMetal::ObjectMotionFrame& frame)
                : mFrame(frame)
            {
            }

            ~Pass()
            {
                if (!mBegun)
                {
                    return;
                }
                for (LLGLSLShader* shader : mUsed)
                {
                    if (shader)
                    {
                        shader->unbindTexture(LLShaderMgr::DEFERRED_DEPTH);
                    }
                }
                LLGLSLShader::unbind();
                LLVertexBuffer::unbind();
                sTarget.flush();
                gGLLastMatrix = nullptr;
                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.loadMatrix(gGLModelView);
            }

            LLGLSLShader& use(LLGLSLShader& shader, F32 reactive = REACTIVE_NONE)
            {
                begin();
                if (&shader != mCurrent)
                {
                    mCurrent = &shader;
                    shader.bind();
                    mSlot = (size_t)(std::find(mUsed.begin(), mUsed.end(), &shader) - mUsed.begin());
                    if (mSlot == mUsed.size())
                    {
                        mSlot = (size_t)(std::find(mUsed.begin(), mUsed.end(), nullptr) - mUsed.begin());
                        llassert(mSlot < mUsed.size());
                        if (mSlot < mUsed.size())
                        {
                            mUsed[mSlot] = &shader;
                            mReactive[mSlot] = REACTIVE_NONE;
                        }
                        shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &gPipeline.mRT->deferredScreen, true,
                                           LLTexUnit::TFO_POINT);
                        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)mFrame.width, (F32)mFrame.height);
                        shader.uniformMatrix4fv(viewProjectionName(), 1, GL_FALSE, mFrame.viewProjection);
                        shader.uniformMatrix4fv(lastViewProjectionName(), 1, GL_FALSE, mFrame.lastViewProjection);
                        const glm::mat4 projection = get_current_projection();
                        shader.uniform2f(depthTermsName(), projection[2][2], projection[3][2]);
                        shader.uniform1f(reactiveName(), REACTIVE_NONE);
                    }
                }
                if (mSlot >= mReactive.size() || mReactive[mSlot] != reactive)
                {
                    if (mSlot < mReactive.size())
                    {
                        mReactive[mSlot] = reactive;
                    }
                    shader.uniform1f(reactiveName(), reactive);
                }
                return shader;
            }

        private:
            void begin()
            {
                if (mBegun)
                {
                    return;
                }
                mBegun = true;
                sTarget.bindTarget();
                F32 clear[4];
                LLRenderTarget::getClearColor(clear[0], clear[1], clear[2], clear[3]);
                const F32 none = (F32)LUMINOUS_TONE_OBJECT_MOTION_NONE;
                LLRenderTarget::setClearColor(none, none, 0.f, 0.f);
                sTarget.clear(); // colour and depth: the depth test's state of the caller writes depth
                LLRenderTarget::setClearColor(clear[0], clear[1], clear[2], clear[3]);
                // the view alone: rigged draws carry no model matrix, and the others' go on it (applyModelMatrix)
                gGLLastMatrix = nullptr;
                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.loadMatrix(gGLModelView);
            }

            const LuminousMetal::ObjectMotionFrame& mFrame;
            LLGLSLShader* mCurrent = nullptr;
            std::array<LLGLSLShader*, 4> mUsed = {}; // the programs that have this frame's uniforms: one of each kind
            std::array<F32, 4> mReactive = {};       // each one's luminous_reactive
            size_t mSlot = 0;                        // mCurrent's in mUsed
            bool mBegun = false;
        };

        // An avatar's entry as this frame sees it: whether it was seen in the frame before (for this avatar), whether
        // it is a first sight (not seen then, or its root joint moved farther since than sModelJump), and its render
        // position now and in the frame before. Its root's place in the agent frame and its render position are taken
        // at its first look-up in the frame and kept for the next.
        const AvatarRoot& seeAvatarRoot(LLVOAvatar* avatar)
        {
            auto [entry, added] = sAvatarRoots.try_emplace(avatar);
            AvatarRoot& root = entry->second;
            if (!added && root.frame == gFrameCount)
            {
                return root;
            }
            const bool follows = !added && root.frame + 1 == gFrameCount && root.avatar == avatar->getID();
            LLJoint* joint = avatar->getRootJoint();
            const LLVector3 place = joint ? joint->getWorldMatrix().getTranslation() : LLVector3::zero;
            root.jumped = !joint || !follows || (place - root.place).lengthSquared() > sModelJump * sModelJump;
            root.follows = follows;
            root.avatar = avatar->getID();
            root.place = place;
            root.lastRender = root.render;
            root.render = avatar->getRenderPosition();
            root.frame = gFrameCount;
            return root;
        }

        // Whether an avatar is a first sight in this frame (seeAvatarRoot).
        bool avatarJumped(LLVOAvatar* avatar)
        {
            return seeAvatarRoot(avatar).jumped;
        }

        // A draw's texture matrix's step in this frame's object update: null where it has no texture matrix, the matrix
        // did not step, was made in the step, or holds the value it held before it.
        const TextureStep* textureStep(const LLMatrix4* matrix)
        {
            if (!matrix || sTextureStepOrder.empty())
            {
                return nullptr;
            }
            auto found = std::lower_bound(sTextureStepOrder.begin(), sTextureStepOrder.end(), matrix,
                                          [](U32 index, const LLMatrix4* address)
                                          { return addressBefore(sTextureSteps[index].matrix, address); });
            if (found == sTextureStepOrder.end())
            {
                return nullptr;
            }
            const TextureStep& step = sTextureSteps[*found];
            if (step.matrix != matrix || step.made
                || std::memcmp(step.last.mMatrix, matrix->mMatrix, sizeof(matrix->mMatrix)) == 0)
            {
                return nullptr;
            }
            return &step;
        }

        // A rigged draw, where its palette moved since the last frame and its avatar did not jump: at a new (avatar,
        // skin) both palettes are found, and uploaded at the run's first draw. Reactive where reactive is set and its
        // texture matrix stepped by a frame.
        void drawSkinned(LLDrawInfo& params, Skin& skin, bool reactive, Pass& pass, Counts& counts)
        {
            LLVOAvatar* avatar = params.mAvatar.get();
            const LLMeshSkinInfo* info = params.mSkinInfo.get();
            if (!params.mCount || params.mVertexBuffer.isNull() || !avatar || !info)
            {
                return;
            }
            const LLVOAvatar::MatrixPaletteCache* palette = nullptr;
            if (avatar != skin.avatar || info->mHash != skin.hash)
            {
                skin = Skin();
                skin.avatar = avatar;
                skin.hash = info->mHash;
                const bool jumped = avatarJumped(avatar);
                palette = &avatar->updateSkinInfoMatrixPalette(info);
                if (jumped || palette->mMatrixPalette.empty() || !palette->mLastValid)
                {
                    ++counts.unseen;
                }
                else if (!palette->mMoved)
                {
                    ++counts.still;
                }
                else
                {
                    skin.moved = true;
                }
            }
            if (!skin.moved)
            {
                return;
            }
            const TextureStep* step = reactive ? textureStep(params.mTextureMatrix) : nullptr;
            const bool frame_step = step && !step->smooth;
            LLGLSLShader& shader = pass.use(gLuminousSkinnedMotionProgram, frame_step ? REACTIVE_MARK : REACTIVE_NONE);
            LLRenderPass::applyModelMatrix(nullptr);
            if (!skin.uploaded && palette)
            {
                const U32 count = (U32)palette->mMatrixPalette.size();
                shader.uniformMatrix3x4fv(LLViewerShaderMgr::AVATAR_MATRIX, count, GL_FALSE, palette->mGLMp.data());
                shader.uniformMatrix3x4fv(lastPaletteName(), count, GL_FALSE, palette->mLastGLMp.data());
                skin.uploaded = true;
                ++counts.skins;
            }
            params.mVertexBuffer->setBuffer();
            params.mVertexBuffer->drawRange(LLRender::TRIANGLES, params.mStart, params.mEnd, params.mCount,
                                            params.mOffset);
            ++counts.draws;
            if (frame_step)
            {
                ++counts.textureFrameDraws;
                ++counts.reactiveDraws;
            }
        }

        // This pass's region matrices: what a static object is drawn through.
        void collectRegionMatrices()
        {
            sRegionMatrices.clear();
            if (LLWorld::instanceExists())
            {
                for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
                {
                    if (region)
                    {
                        sRegionMatrices.push_back(&region->mRenderMatrix);
                    }
                }
            }
            std::sort(sRegionMatrices.begin(), sRegionMatrices.end(), std::less<const LLMatrix4*>());
        }

        // The entries of a table not seen for MODEL_KEEP frames go.
        template <typename Table>
        void sweep(Table& table)
        {
            for (auto it = table.begin(); it != table.end();)
            {
                if (gFrameCount - it->second.frame > MODEL_KEEP)
                {
                    it = table.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Now and then, the model matrices', the avatars' roots' and the system avatar parts' tables are swept.
        void sweepTables()
        {
            if (gFrameCount - sModelsSwept < MODEL_SWEEP)
            {
                return;
            }
            sModelsSwept = gFrameCount;
            sweep(sModels);
            sweep(sAvatarRoots);
            sweep(sAvatarPalettes);
        }

        // How far a model matrix or an avatar's root may have moved since the last frame and still give its motion:
        // the last frame is the one before this (objectMotionWanted), gFrameIntervalSeconds ago.
        F32 modelJump()
        {
            return MODEL_JUMP + MODEL_SPEED * llclamp(gFrameIntervalSeconds.value(), 0.f, MODEL_TIME_MOST);
        }

        // A draw's model matrix as this frame sees it: null for none and for a region's own; otherwise its entry in the
        // table, brought up to this frame at its first look-up in it, and not moved where first_sight is set.
        const Model* seeModel(const LLMatrix4* matrix, bool first_sight, Counts& counts)
        {
            if (!matrix
                || std::binary_search(sRegionMatrices.begin(), sRegionMatrices.end(), matrix,
                                      std::less<const LLMatrix4*>()))
            {
                return nullptr;
            }
            auto [entry, added] = sModels.try_emplace(matrix);
            Model& model = entry->second;
            if (!added && model.frame == gFrameCount)
            {
                return &model;
            }
            const bool follows = !first_sight && !added && model.frame + 1 == gFrameCount;
            model.last = model.now;
            model.now = *matrix;
            model.frame = gFrameCount;
            model.moved = false;
            if (!follows)
            {
                ++counts.modelsUnseen;
            }
            else if (std::memcmp(model.now.mMatrix, model.last.mMatrix, sizeof(model.now.mMatrix)) == 0)
            {
                ++counts.modelsStill;
            }
            else if ((model.now.getTranslation() - model.last.getTranslation()).lengthSquared()
                     > sModelJump * sModelJump)
            {
                ++counts.modelsUnseen;
            }
            else
            {
                model.moved = true;
                ++counts.models;
            }
            return &model;
        }

        // A draw through a model matrix, where it moved since the last frame, where it is reactive, or where it has a
        // texture's smooth step (texture_now and texture_last, its texture matrix of this frame and the last): at a new
        // matrix its entry is found (a first sight where first_sight is set), and its matrices uploaded to the program
        // the draw takes and the model view set at the run's first draw with it. A run whose matrix moved has this
        // frame's and the last frame's; any other (a matrix that did not move, a first sight, a region's, none) has
        // this frame's as both, so its reactive and texture draws give the camera's motion, the latter with the
        // texture's own added. A texture step's draw takes the texture program with both texture matrices, any other
        // the plain one. Whether it drew.
        bool drawThrough(const LLMatrix4* matrix, bool first_sight, bool reactive, const LLMatrix4* texture_now,
                         const LLMatrix4* texture_last, LLVertexBuffer* buffer, U32 start, U32 end, U32 count,
                         U32 offset, ModelRun& run, Pass& pass, Counts& counts)
        {
            if (matrix != run.matrix)
            {
                run.matrix = matrix;
                run.model = seeModel(run.matrix, first_sight, counts);
                run.uploaded = false;
                run.textureUploaded = false;
            }
            const bool moved = run.model && run.model->moved;
            const bool texture = texture_now && texture_last;
            if ((!moved && !reactive && !texture) || !count || !buffer)
            {
                return false;
            }
            LLGLSLShader& shader = pass.use(texture ? gLuminousTextureMotionProgram : gLuminousMotionProgram,
                                            reactive ? REACTIVE_MARK : REACTIVE_NONE);
            bool& uploaded = texture ? run.textureUploaded : run.uploaded;
            if (!uploaded)
            {
                if (moved)
                {
                    shader.uniformMatrix4fv(modelName(), 1, GL_FALSE, &run.model->now.mMatrix[0][0]);
                    shader.uniformMatrix4fv(lastModelName(), 1, GL_FALSE, &run.model->last.mMatrix[0][0]);
                }
                else
                {
                    static const LLMatrix4 identity;
                    const LLMatrix4& now = run.matrix ? *run.matrix : identity;
                    shader.uniformMatrix4fv(modelName(), 1, GL_FALSE, &now.mMatrix[0][0]);
                    shader.uniformMatrix4fv(lastModelName(), 1, GL_FALSE, &now.mMatrix[0][0]);
                }
                uploaded = true;
                ++counts.modelRuns;
            }
            if (texture)
            {
                // as the pools give texture_matrix0
                shader.uniformMatrix4fv(textureNowName(), 1, GL_FALSE, &texture_now->mMatrix[0][0]);
                shader.uniformMatrix4fv(textureLastName(), 1, GL_FALSE, &texture_last->mMatrix[0][0]);
            }
            LLRenderPass::applyModelMatrix(run.matrix);
            buffer->setBuffer();
            buffer->drawRange(LLRender::TRIANGLES, start, end, count, offset);
            if (moved)
            {
                ++counts.modelDraws;
            }
            if (reactive)
            {
                ++counts.reactiveDraws;
            }
            return true;
        }

        // This pass's ranges of vertices of the reshaped flexible prims' faces: those of the drawables that live, each
        // face's [geometry index, geometry index + geometry count - 1] in its vertex buffer, sorted by buffer.
        void collectReactiveRanges()
        {
            sReactiveRanges.clear();
            for (const LLPointer<LLDrawable>& drawable : sReshaped)
            {
                if (drawable.isNull() || drawable->isDead())
                {
                    continue;
                }
                for (S32 i = 0; i < drawable->getNumFaces(); ++i)
                {
                    const LLFace* face = drawable->getFace(i);
                    LLVertexBuffer* buffer = face ? face->getVertexBuffer() : nullptr;
                    if (!buffer || face->getGeomCount() == 0)
                    {
                        continue;
                    }
                    ReactiveRange range;
                    range.buffer = buffer;
                    range.start = face->getGeomIndex();
                    range.end = range.start + face->getGeomCount() - 1;
                    sReactiveRanges.push_back(range);
                }
            }
            std::sort(sReactiveRanges.begin(), sReactiveRanges.end(), [](const ReactiveRange& a, const ReactiveRange& b)
                      { return std::less<const LLVertexBuffer*>()(a.buffer, b.buffer); });
        }

        // Whether a draw that is not rigged is reactive: its vertices in its buffer overlap a reshaped flexible prim's
        // face's.
        bool reactiveDraw(const LLDrawInfo& params)
        {
            if (sReactiveRanges.empty() || params.mVertexBuffer.isNull())
            {
                return false;
            }
            const LLVertexBuffer* buffer = params.mVertexBuffer.get();
            auto range = std::lower_bound(sReactiveRanges.begin(), sReactiveRanges.end(), buffer,
                                          [](const ReactiveRange& r, const LLVertexBuffer* b)
                                          { return std::less<const LLVertexBuffer*>()(r.buffer, b); });
            for (; range != sReactiveRanges.end() && range->buffer == buffer; ++range)
            {
                if (params.mStart <= range->end && range->start <= params.mEnd)
                {
                    return true;
                }
            }
            return false;
        }

        // A draw that is not rigged, through its model matrix; reactive where it overlaps a reshaped flexible prim.
        // Where its texture matrix stepped in this frame: a smooth step of an opaque draw (opaque), where the texture
        // program is ready (texture_ready), with it and the texture's own motion; a frame step, a smooth step of a
        // cut-out or alpha draw, or a smooth one without that program, reactive where reactive is set.
        void drawModel(LLDrawInfo& params, bool opaque, bool reactive, bool texture_ready, ModelRun& run, Pass& pass,
                       Counts& counts)
        {
            const TextureStep* step = textureStep(params.mTextureMatrix);
            const bool smooth = step && step->smooth && opaque && texture_ready;
            const bool frame_step = step && !smooth && reactive;
            if (drawThrough(params.mModelMatrix, false, frame_step || reactiveDraw(params),
                            smooth ? params.mTextureMatrix : nullptr, smooth ? &step->last : nullptr,
                            params.mVertexBuffer.get(), params.mStart, params.mEnd, params.mCount, params.mOffset, run,
                            pass, counts))
            {
                if (smooth)
                {
                    ++counts.textureSmoothDraws;
                }
                else if (frame_step)
                {
                    ++counts.textureFrameDraws;
                }
            }
        }

        // Whether the avatar pool draws the avatar in full in the main view, as LLDrawPoolAvatar::renderAvatars decides
        // for its rigid and skinned passes: its pool has its faces in this view's cull, it is fully loaded, not left
        // out by RenderAvatarFriendsOnly, not impostored, its appearance normal or, where it is jelly-dolled or
        // invisible, its impostor to be made again (needsImpostorUpdate: then it is drawn in full, its parts as
        // luminousMotionParts picks them), and for an animesh worn by an avatar, that avatar's appearance normal and
        // avatars drawn.
        bool avatarDrawn(LLVOAvatar* avatar, bool friends_only)
        {
            if (!avatar || avatar->isDead() || avatar->mDrawable.isNull() || avatar->isUIAvatar()
                || avatar->mDrawable->getNumFaces() == 0)
            {
                return false;
            }
            LLFace* face = avatar->mDrawable->getFace(0);
            LLFacePool* pool = face ? face->getPool() : nullptr;
            if (!pool || pool->mDrawFace.empty() || pool->getSkipRenderFlag()
                || !gPipeline.hasRenderType(pool->getType()))
            {
                return false;
            }
            if (!avatar->isFullyLoaded()
                || (friends_only && !avatar->isControlAvatar() && !avatar->isSelf() && !avatar->isBuddy())
                || avatar->isImpostor()
                || (avatar->getOverallAppearance() != LLVOAvatar::AOA_NORMAL && !avatar->needsImpostorUpdate()))
            {
                return false;
            }
            LLVOAvatar* attached = avatar->getAttachedAvatar();
            return !attached
                   || (attached->getOverallAppearance() == LLVOAvatar::AOA_NORMAL
                       && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_AVATAR));
        }

        // The joint meshes LLViewerJoint::render draws under a part: the joint's own shape where it is valid, then its
        // children, each where the pixel area reaches its level of detail, stopping after the first with a level of
        // its own. A mesh is kept where drawShape would draw it.
        void collectMeshes(LLAvatarJoint* joint, F32 pixel_area, const LLVOAvatar* avatar, bool jumped)
        {
            if (joint->getValid())
            {
                if (LLViewerJointMesh* mesh = dynamic_cast<LLViewerJointMesh*>(joint))
                {
                    AvatarPart part;
                    part.mesh = mesh;
                    part.avatar = avatar;
                    part.jumped = jumped;
                    if (mesh->luminousMotionDraw(part.draw))
                    {
                        sAvatarParts.push_back(part);
                    }
                }
            }
            for (LLJoint* child : joint->mChildren)
            {
                LLAvatarJoint* child_joint = static_cast<LLAvatarJoint*>(child);
                const F32 lod = child_joint->getLOD();
                if (pixel_area >= lod || LLAvatarJoint::sDisableLOD)
                {
                    collectMeshes(child_joint, pixel_area, avatar, jumped);
                    if (lod != DEFAULT_AVATAR_JOINT_LOD)
                    {
                        break;
                    }
                }
            }
        }

        // This pass's system avatar parts: what the avatar pool draws of each avatar it draws in full in this view,
        // with whether the avatar jumped.
        void collectAvatarParts()
        {
            static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
            sAvatarParts.clear();
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
                if (!avatarDrawn(avatar, friends_only()))
                {
                    continue;
                }
                const bool jumped = avatarJumped(avatar);
                const F32 pixel_area = avatar->luminousMotionParts(sAvatarJoints);
                for (LLViewerJoint* joint : sAvatarJoints)
                {
                    collectMeshes(joint, pixel_area, avatar, jumped);
                }
            }
        }

        // A skinned part's joint matrices of this frame, built at its first look-up in the frame, beside the last
        // frame's; a first sight where its avatar jumped.
        const AvatarPalette& seeAvatarPalette(const AvatarPart& part, Counts& counts)
        {
            auto [entry, added] = sAvatarPalettes.try_emplace(part.mesh);
            AvatarPalette& palette = entry->second;
            if (!added && palette.frame == gFrameCount)
            {
                return palette;
            }
            const LLUUID& avatar = part.avatar->getID();
            const bool follows = !part.jumped && !added && palette.frame + 1 == gFrameCount && palette.avatar == avatar;
            palette.now ^= 1;
            part.mesh->luminousJointMatrices(palette.matrices[palette.now]);
            palette.avatar = avatar;
            palette.frame = gFrameCount;
            palette.moved = follows
                            && std::memcmp(palette.matrices[0], palette.matrices[1], sizeof(palette.matrices[0])) != 0;
            if (!follows)
            {
                ++counts.avatarUnseen;
            }
            else if (!palette.moved)
            {
                ++counts.avatarStill;
            }
            else
            {
                ++counts.avatarParts;
            }
            return palette;
        }

        // A skinned system avatar part, where its joint matrices moved since the last frame and its avatar did not
        // jump: both sets uploaded.
        void drawAvatarPart(const AvatarPart& part, Pass& pass, Counts& counts)
        {
            const AvatarPalette& palette = seeAvatarPalette(part, counts);
            if (!palette.moved || !part.draw.count)
            {
                return;
            }
            LLGLSLShader& shader = pass.use(gLuminousAvatarMotionProgram);
            LLRenderPass::applyModelMatrix(nullptr);
            shader.uniform4fv(LLViewerShaderMgr::AVATAR_MATRIX, JOINT_FLOATS / 4, palette.matrices[palette.now]);
            shader.uniform4fv(lastPaletteName(), JOINT_FLOATS / 4, palette.matrices[palette.now ^ 1]);
            part.draw.buffer->setBuffer();
            part.draw.buffer->drawRange(LLRender::TRIANGLES, part.draw.start, part.draw.end, part.draw.count,
                                        part.draw.offset);
            ++counts.avatarDraws;
        }

        // Whether the avatar pool draws the avatar as its impostor in the main view, as LLDrawPoolAvatar::renderAvatars
        // decides for its first pass: its pool has its faces in this view's cull, it is fully loaded, not left out by
        // RenderAvatarFriendsOnly, and impostored, or its appearance not normal and its impostor not to be made again
        // (then not with the camera under water); for an animesh worn by an avatar, that avatar's appearance normal and
        // avatars drawn; and its impostor made (renderImpostor). Never the agent's own avatar, nor an animesh it wears.
        bool impostorDrawn(LLVOAvatar* avatar, bool friends_only)
        {
            if (!avatar || avatar->isDead() || avatar->mDrawable.isNull() || avatar->isUIAvatar() || avatar->isSelf()
                || avatar->mDrawable->getNumFaces() == 0 || !avatar->mImpostor.isComplete())
            {
                return false;
            }
            LLFace* face = avatar->mDrawable->getFace(0);
            LLFacePool* pool = face ? face->getPool() : nullptr;
            if (!pool || pool->mDrawFace.empty() || pool->getSkipRenderFlag()
                || !gPipeline.hasRenderType(pool->getType()))
            {
                return false;
            }
            if (!avatar->isFullyLoaded() || (friends_only && !avatar->isControlAvatar() && !avatar->isBuddy()))
            {
                return false;
            }
            if (!avatar->isImpostor()
                && (avatar->getOverallAppearance() == LLVOAvatar::AOA_NORMAL || avatar->needsImpostorUpdate()
                    || LLPipeline::sUnderWaterRender))
            {
                return false;
            }
            LLVOAvatar* attached = avatar->getAttachedAvatar();
            return !attached
                   || (!attached->isSelf() && attached->getOverallAppearance() == LLVOAvatar::AOA_NORMAL
                       && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_AVATAR));
        }

        // Whether the avatar's impostor was made again in this frame (impostorRefreshed).
        bool impostorRefreshedNow(const LLVOAvatar* avatar)
        {
            for (const auto& [address, id] : sRefreshed)
            {
                if (address == avatar && id == avatar->getID())
                {
                    return true;
                }
            }
            return false;
        }

        // An impostored avatar's quad, with the corners renderImpostor gives it in this frame: where its render
        // position moved since the last frame and it is not a first sight (not seen in the frame before, or moved
        // farther than sModelJump; its root joint, which an impostor moves only now and then, is not asked), with that
        // move; where reactive is set, drawn reactive (REACTIVE_MARK), with no move where it did not move or is a
        // first sight. The corners are in the agent frame and go through the view alone, as renderImpostor's do; this
        // frame's model matrix is the identity and the last frame's a translation by the render position's change. The
        // plain program's model matrices are then the quad's, no longer the run's.
        void drawImpostor(LLVOAvatar* avatar, bool reactive, ModelRun& run, Pass& pass, Counts& counts)
        {
            const AvatarRoot& root = seeAvatarRoot(avatar);
            const LLVector3 move = root.lastRender - root.render; // the last frame's place minus this frame's
            const bool unseen = !root.follows || move.lengthSquared() > sModelJump * sModelJump;
            const bool moved = !unseen && root.render != root.lastRender;
            if (unseen)
            {
                ++counts.impostorsUnseen;
            }
            else if (moved)
            {
                ++counts.impostorsMoved;
            }
            else
            {
                ++counts.impostorsStill;
            }
            if (!moved && !reactive)
            {
                return;
            }
            LLGLSLShader& shader = pass.use(gLuminousMotionProgram, reactive ? REACTIVE_MARK : REACTIVE_NONE);
            LLRenderPass::applyModelMatrix(nullptr);
            static const LLMatrix4 identity;
            LLMatrix4 last;
            if (moved)
            {
                last.setTranslation(move);
            }
            shader.uniformMatrix4fv(modelName(), 1, GL_FALSE, &identity.mMatrix[0][0]);
            shader.uniformMatrix4fv(lastModelName(), 1, GL_FALSE, &last.mMatrix[0][0]);
            run.uploaded = false;

            const LLViewerCamera* camera = LLViewerCamera::getInstance();
            const LLVector3 pos(root.render + avatar->getImpostorOffset());
            LLVector3 at = pos - camera->getOrigin();
            at.normalize();
            LLVector3 left = camera->getUpAxis() % at;
            LLVector3 up = at % left;
            left *= avatar->getImpostorDim().mV[0];
            up *= avatar->getImpostorDim().mV[1];
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex3fv((pos + left - up).mV);
            gGL.vertex3fv((pos - left - up).mV);
            gGL.vertex3fv((pos - left + up).mV);
            gGL.vertex3fv((pos + left - up).mV);
            gGL.vertex3fv((pos - left + up).mV);
            gGL.vertex3fv((pos + left + up).mV);
            gGL.end();
            gGL.flush(); // drawn with this quad's model matrices, before the next quad sets its own
            ++counts.impostorDraws;
            if (reactive)
            {
                ++counts.impostorsReactive;
                ++counts.reactiveDraws;
            }
        }

        // The quads of the avatars the avatar pool draws as their impostor in this view; reactive where reactive is set
        // and the impostor was made again in this frame.
        void drawImpostors(bool reactive, ModelRun& run, Pass& pass, Counts& counts)
        {
            static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
                if (impostorDrawn(avatar, friends_only()))
                {
                    drawImpostor(avatar, reactive && impostorRefreshedNow(avatar), run, pass, counts);
                }
            }
        }

        // The draws of an alpha group's pass (PASS_ALPHA or PASS_ALPHA_RIGGED), as the alpha pool picks its groups.
        LLSpatialGroup::drawmap_elem_t* alphaDraws(LLSpatialGroup* group, U32 type)
        {
            if (!group || group->isDead() || !group->getSpatialPartition()->mRenderByGroup)
            {
                return nullptr;
            }
            LLSpatialGroup::draw_map_t::iterator found = group->mDrawMap.find(type);
            return found == group->mDrawMap.end() ? nullptr : &found->second;
        }

        // Alpha that is not rigged writes its depth, in the main view (lldrawpoolalpha.cpp), in the pre-water pool,
        // and in every group when depth of field draws the post-water groups' depth (where at least a third covers) or
        // the water takes no copy of the screen; elsewhere its depth is not in the frame. Whether the pre-water pool
        // draws the group, as LLDrawPoolAlpha::renderAlphaGroups picks them: some part of it beyond the water from the
        // camera (below the water, or above it from under the water).
        bool preWaterGroup(LLSpatialGroup* group, F32 water_height)
        {
            LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : group->getExtents();
            return LLPipeline::sUnderWaterRender ? ext[1].getF32ptr()[2] >= water_height
                                                 : ext[0].getF32ptr()[2] <= water_height;
        }

        bool programReady(const LLGLSLShader& shader)
        {
            return shader.isComplete() && shader.isReflectionReady() && shader.isPipelineReady();
        }

        // A program's readiness for the log: 0 ready, 1 not built (failed or unloaded), 2 its reflection not published
        // yet, 3 its pipeline not compiled yet.
        U32 programState(const LLGLSLShader& shader)
        {
            return !shader.isComplete() ? 1 : !shader.isReflectionReady() ? 2 : !shader.isPipelineReady() ? 3 : 0;
        }

        std::string programStates()
        {
            static const char* const STATES[] = { "ready", "not built (failed or unloaded)",
                                                  "its reflection not published", "its pipeline not compiled" };
            return llformat("%s %s, %s %s, %s %s, %s %s", gLuminousSkinnedMotionProgram.mName.c_str(),
                            STATES[programState(gLuminousSkinnedMotionProgram)],
                            gLuminousAvatarMotionProgram.mName.c_str(),
                            STATES[programState(gLuminousAvatarMotionProgram)], gLuminousMotionProgram.mName.c_str(),
                            STATES[programState(gLuminousMotionProgram)], gLuminousTextureMotionProgram.mName.c_str(),
                            STATES[programState(gLuminousTextureMotionProgram)]);
        }

        // Why the pass did not draw, in the main view of frames drawn for the scaler, for one line in the log: a reason
        // is said once it has held for QUIET_FRAMES such frames in a row, or when that many have gone without a draw,
        // and not again while it is the one last said; the first draw after a reason other than NOTHING_MOVED says so.
        // A reason of a frame or two (the history reset at a camera cut, a snapshot's frame between) is not said, nor
        // is one that comes and goes with draws in between.
        constexpr U32 QUIET_FRAMES = 30;
        const char* const SWITCHED_OFF = "switched off (LuminousMetalFXObjectMotion)";
        const char* const NO_PROGRAM = "no motion program is ready";
        const char* const NO_TARGET = "no motion target";
        const char* const NOTHING_MOVED = "nothing moved since the last frame";
        struct Quiet
        {
            const char* said = nullptr; // the reason last said; null: none, or a draw said since
            U32 saidCode = 0;
            const char* now = nullptr;  // the reason of the frames in a row before this one
            U32 nowCode = 0;
            U32 frames = 0;             // how many
            U32 notDrawn = 0;           // frames drawn for the scaler in a row without a draw
        };
        Quiet sQuiet;

        // A frame in which the pass did not draw: why, a code that tells apart what the reason covers (the programs'
        // states), and the line's detail, made only when it is said.
        template <typename Detail>
        void notDrawn(const LuminousMetal::ObjectMotionFrame& frame, const char* why, U32 code, Detail detail)
        {
            if (!frame.scalerView || !why)
            {
                return;
            }
            ++sQuiet.notDrawn;
            if (why == sQuiet.now && code == sQuiet.nowCode)
            {
                ++sQuiet.frames;
            }
            else
            {
                sQuiet.now = why;
                sQuiet.nowCode = code;
                sQuiet.frames = 1;
            }
            if ((sQuiet.frames == QUIET_FRAMES || sQuiet.notDrawn == QUIET_FRAMES)
                && (why != sQuiet.said || code != sQuiet.saidCode))
            {
                sQuiet.said = why;
                sQuiet.saidCode = code;
                LL_INFOS("LuminousMetal") << "MetalFX object motion does not draw in frames drawn for the scaler ("
                                          << sQuiet.notDrawn << " in a row): " << why << "; " << detail() << LL_ENDL;
            }
        }

        // A frame in which the pass drew.
        void drawn(const LuminousMetal::ObjectMotionFrame& frame, const Counts& counts)
        {
            if (sQuiet.said && sQuiet.said != NOTHING_MOVED)
            {
                LL_INFOS("LuminousMetal") << "MetalFX object motion draws again at " << frame.width
                                          << "x" << frame.height
                                          << " after " << sQuiet.notDrawn << " frames drawn for the scaler without it: "
                                          << counts.draws << " rigged draws, " << counts.avatarDraws
                                          << " draws of system avatar parts, " << counts.modelDraws
                                          << " draws through moving model matrices, " << counts.impostorDraws
                                          << " impostors' quads, " << counts.textureSmoothDraws
                                          << " draws of smooth texture steps, " << counts.textureFrameDraws
                                          << " of texture frame steps, " << counts.reactiveDraws << " reactive draws; "
                                          << programStates() << LL_ENDL;
                sQuiet.said = nullptr;
            }
            sQuiet.now = nullptr;
            sQuiet.frames = 0;
            sQuiet.notDrawn = 0;
        }

        void sayTiming()
        {
            const F64 passes = (F64)sTiming.passes;
            LL_INFOS("LuminousMetal") << "MetalFX object motion over " << sTiming.passes << " passes: main thread "
                                      << llformat("%.0f", (F64)sTiming.total / passes) << " us a pass on average, "
                                      << sTiming.most << " us at most; a pass drew "
                                      << llformat("%.1f", (F64)sTiming.draws / passes) << " rigged draws of "
                                      << llformat("%.1f", (F64)sTiming.skins / passes) << " moving skins, "
                                      << llformat("%.1f", (F64)sTiming.avatarDraws / passes) << " draws of "
                                      << llformat("%.1f", (F64)sTiming.avatarParts / passes)
                                      << " moving system avatar parts and "
                                      << llformat("%.1f", (F64)sTiming.modelDraws / passes) << " draws of "
                                      << llformat("%.1f", (F64)sTiming.models / passes)
                                      << " moving model matrices, of "
                                      << llformat("%.1f", (F64)sTiming.modelsSeen / passes) << " model matrices seen, "
                                      << llformat("%.1f", (F64)sTiming.impostorDraws / passes) << " impostors' quads, "
                                      << llformat("%.1f", (F64)sTiming.textureSmoothDraws / passes)
                                      << " draws of smooth texture steps, "
                                      << llformat("%.1f", (F64)sTiming.textureFrameDraws / passes)
                                      << " of texture frame steps, and "
                                      << llformat("%.1f", (F64)sTiming.reactiveDraws / passes) << " reactive draws"
                                      << LL_ENDL;
            sTiming = Timing();
        }
    }

    void draw()
    {
        static LLCachedControl<bool> on(gSavedSettings, "LuminousMetalFXObjectMotion", true);
        static LLCachedControl<bool> timing(gSavedSettings, "LuminousMetalPassTimes", false);
        static LLCachedControl<F32> reactive_strength(gSavedSettings, "LuminousMetalFXObjectReactive", 0.5f);
        if (!LuminousMetal::metal())
        {
            return;
        }
        if (!timing() && sTiming.passes > 0)
        {
            sayTiming();
        }
        LuminousMetal::ObjectMotionFrame frame;
        const bool wanted = LuminousMetal::objectMotionWanted(frame);
        if (!on())
        {
            if (sTarget.isComplete())
            {
                sTarget.release();
            }
            sModels.clear();
            sAvatarRoots.clear();
            sAvatarPalettes.clear();
            notDrawn(frame, SWITCHED_OFF, 0, []() { return std::string("the camera's motion only"); });
            return;
        }
        if (!wanted)
        {
            notDrawn(frame, frame.refusal, 0, []() { return LuminousMetal::objectMotionState(); });
            return;
        }
        LLGLSLShader& skinned = gLuminousSkinnedMotionProgram;
        LLGLSLShader& avatar = gLuminousAvatarMotionProgram;
        LLGLSLShader& plain = gLuminousMotionProgram;
        const bool skinned_ready = programReady(skinned);
        const bool avatar_ready = programReady(avatar);
        const bool plain_ready = programReady(plain);
        const bool texture_ready = programReady(gLuminousTextureMotionProgram); // used among the plain one's draws
        if (!skinned_ready && !avatar_ready && !plain_ready)
        {
            notDrawn(frame, NO_PROGRAM,
                     programState(skinned) | (programState(plain) << 2) | (programState(avatar) << 4)
                         | (programState(gLuminousTextureMotionProgram) << 6),
                     []() { return programStates(); });
            return;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
        LL_PROFILE_GPU_ZONE("luminous object motion");
        const U64 started = timing() ? (U64)totalTime() : 0;

        if (sTarget.getWidth() != frame.width || sTarget.getHeight() != frame.height || !sTarget.getRHITexture(0))
        {
            sTarget.release();
            sTarget.setMemorylessDepth(true); // the pass's own depth is never read after it
            if (!sTarget.allocate(frame.width, frame.height, GL_RGBA16F, true) || !sTarget.getRHITexture(0))
            {
                sTarget.release();
                if (!sAllocationRefused)
                {
                    sAllocationRefused = true;
                    LL_WARNS("LuminousMetal") << "MetalFX object motion: no target of " << frame.width
                                              << "x" << frame.height
                                              << "; the camera's motion only" << LL_ENDL;
                }
                notDrawn(frame, NO_TARGET, 0,
                         [&frame]() { return llformat("RGBA16F with a memoryless depth at %ux%u could not be made",
                                                      frame.width, frame.height); });
                return;
            }
        }

        bool colour_r = true;
        bool colour_g = true;
        bool colour_b = true;
        bool colour_a = false;
        gGL.getColorMask(colour_r, colour_g, colour_b, colour_a);
        Counts counts;
        sweepTables();
        sModelJump = modelJump();
        // The reactive draws, only where a mask stage can take them.
        const F32 strength = llclamp((F32)reactive_strength(), 0.f, 1.f);
        const bool reactive = strength > 0.f && (!sReshaped.empty() || !sRefreshed.empty() || !sTextureSteps.empty())
                              && LuminousMetal::objectReactiveWanted();
        sReactiveRanges.clear();
        if (reactive && plain_ready && !sReshaped.empty())
        {
            collectReactiveRanges();
            counts.reactiveRanges = (U32)sReactiveRanges.size();
        }
        sortTextureSteps();
        counts.textureSmooth = sTextureStepCounts.smooth;
        counts.textureFrame = sTextureStepCounts.frame;
        counts.textureMade = sTextureStepCounts.made;
        {
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LESS);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable cull(GL_CULL_FACE);
            LLGLDisable offset(GL_POLYGON_OFFSET_FILL);
            gGL.setColorMask(true, true);

            Pass pass(frame); // ends before the state above is put back
            if (skinned_ready)
            {
                Skin skin;
                for (U32 type : TYPES)
                {
                    const U32 rigged = type + 1;
                    LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(rigged);
                    for (LLCullResult::drawinfo_iterator i = gPipeline.beginRenderMap(rigged); i != end;)
                    {
                        LLDrawInfo* params = *i;
                        LLCullResult::increment_iterator(i, end);
                        if (params)
                        {
                            drawSkinned(*params, skin, reactive, pass, counts);
                        }
                    }
                }
                // Worn alpha (mesh hair, lashes): its depth is in the frame's where it covers enough
                // (lldrawpoolalpha.cpp), so a fragment is kept there; elsewhere what lies behind it keeps its own
                // motion.
                LLCullResult::sg_iterator groups_end = gPipeline.endRiggedAlphaGroups();
                for (LLCullResult::sg_iterator g = gPipeline.beginRiggedAlphaGroups(); g != groups_end; ++g)
                {
                    LLSpatialGroup::drawmap_elem_t* draws = alphaDraws(*g, LLRenderPass::PASS_ALPHA_RIGGED);
                    if (!draws)
                    {
                        continue;
                    }
                    for (LLSpatialGroup::drawmap_elem_t::iterator k = draws->begin(); k != draws->end(); ++k)
                    {
                        LLDrawInfo* params = *k;
                        if (params && params->mAvatar.notNull())
                        {
                            drawSkinned(*params, skin, reactive, pass, counts);
                        }
                    }
                }
            }
            // The system avatar: its skinned parts with the avatar program, its eyeballs with the models below.
            if (avatar_ready || plain_ready)
            {
                collectAvatarParts();
            }
            if (avatar_ready)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("luminous object motion - system avatars");
                for (const AvatarPart& part : sAvatarParts)
                {
                    if (part.draw.skinned)
                    {
                        drawAvatarPart(part, pass, counts);
                    }
                }
            }
            if (plain_ready)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("luminous object motion - models");
                collectRegionMatrices();
                ModelRun run;
                for (U32 type : TYPES)
                {
                    LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(type);
                    for (LLCullResult::drawinfo_iterator i = gPipeline.beginRenderMap(type); i != end;)
                    {
                        LLDrawInfo* params = *i;
                        LLCullResult::increment_iterator(i, end);
                        if (params)
                        {
                            drawModel(*params, !cutOut(type), reactive, texture_ready, run, pass, counts);
                        }
                    }
                }
                // Alpha: drawn only where the alpha pool writes its depth (preWaterGroup), as a fragment is kept only
                // on the frame's depth. Particles carry no model matrix.
                const bool every_group = LLPipeline::RenderDepthOfField || LLDrawPoolWater::sSkipScreenCopy;
                const F32 water_height = LLEnvironment::instance().getWaterHeight();
                LLCullResult::sg_iterator groups_end = gPipeline.endAlphaGroups();
                for (LLCullResult::sg_iterator g = gPipeline.beginAlphaGroups(); g != groups_end; ++g)
                {
                    LLSpatialGroup::drawmap_elem_t* draws = alphaDraws(*g, LLRenderPass::PASS_ALPHA);
                    const U32 partition = draws ? (*g)->getSpatialPartition()->mPartitionType : 0;
                    if (!draws || partition == LLViewerRegion::PARTITION_PARTICLE
                        || partition == LLViewerRegion::PARTITION_HUD_PARTICLE
                        || (!every_group && !preWaterGroup(*g, water_height)))
                    {
                        continue;
                    }
                    for (LLSpatialGroup::drawmap_elem_t::iterator k = draws->begin(); k != draws->end(); ++k)
                    {
                        LLDrawInfo* params = *k;
                        if (params && params->mAvatar.isNull())
                        {
                            drawModel(*params, false, reactive, texture_ready, run, pass, counts);
                        }
                    }
                }
                // The system avatar's eyeballs, placed by their world matrices as drawShape places them; a first sight
                // where their avatar jumped.
                for (const AvatarPart& part : sAvatarParts)
                {
                    if (!part.draw.skinned)
                    {
                        drawThrough(&part.mesh->getWorldMatrix(), part.jumped, false, nullptr, nullptr,
                                    part.draw.buffer, part.draw.start, part.draw.end, part.draw.count, part.draw.offset,
                                    run, pass, counts);
                    }
                }
                // The impostored avatars' quads, through the immediate mode renderImpostor draws them with.
                drawImpostors(reactive, run, pass, counts);
            }
            sAvatarParts.clear();
        }
        gGL.setColorMask(colour_r, colour_g, colour_b, colour_a);
        sReactiveRanges.clear();
        const bool reactive_staged = counts.reactiveDraws > 0 && LuminousMetal::temporalObjectReactive(strength);

        if (timing())
        {
            const U64 spent = (U64)totalTime() - started;
            ++sTiming.passes;
            sTiming.total += spent;
            sTiming.most = llmax(sTiming.most, spent);
            sTiming.draws += counts.draws;
            sTiming.skins += counts.skins;
            sTiming.modelDraws += counts.modelDraws;
            sTiming.models += counts.models;
            sTiming.modelsSeen += counts.models + counts.modelsStill + counts.modelsUnseen;
            sTiming.avatarDraws += counts.avatarDraws;
            sTiming.avatarParts += counts.avatarParts;
            sTiming.reactiveDraws += counts.reactiveDraws;
            sTiming.impostorDraws += counts.impostorDraws;
            sTiming.textureSmoothDraws += counts.textureSmoothDraws;
            sTiming.textureFrameDraws += counts.textureFrameDraws;
        }

        if (counts.draws + counts.avatarDraws + counts.modelDraws + counts.impostorDraws + counts.reactiveDraws
            + counts.textureSmoothDraws > 0)
        {
            LuminousMetal::objectMotionDrawn();
            drawn(frame, counts);
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass at " << frame.width
                                          << "x" << frame.height
                                          << ", " << counts.draws << " draws of " << counts.skins << " moving skins ("
                                          << counts.still << " still, " << counts.unseen << " without a last frame), "
                                          << counts.avatarDraws << " draws of " << counts.avatarParts
                                          << " moving system avatar parts (" << counts.avatarStill << " still, "
                                          << counts.avatarUnseen << " without a last frame), "
                                          << counts.modelDraws << " draws of " << counts.models
                                          << " moving model matrices (" << counts.modelsStill << " still, "
                                          << counts.modelsUnseen << " without a last frame), " << counts.impostorDraws
                                          << " impostors' quads, " << counts.textureSmoothDraws
                                          << " draws of smooth texture steps, " << counts.textureFrameDraws
                                          << " of texture frame steps, " << counts.reactiveDraws << " reactive draws"
                                          << LL_ENDL;
            }
        }
        else
        {
            notDrawn(frame, NOTHING_MOVED, 0,
                     [&counts]()
                     {
                         return llformat("rigged: %u runs of a skin still, %u without a last frame; system avatar "
                                         "parts: %u still, %u without a last frame; not rigged: %u model matrices "
                                         "still, %u without a last frame; impostors: %u still, %u without a last "
                                         "frame; texture animations: %u smooth steps, %u frame steps, %u made in "
                                         "the step; ",
                                         counts.still, counts.unseen, counts.avatarStill, counts.avatarUnseen,
                                         counts.modelsStill, counts.modelsUnseen, counts.impostorsStill,
                                         counts.impostorsUnseen, counts.textureSmooth, counts.textureFrame,
                                         counts.textureMade)
                                + programStates();
                     });
        }
        if (counts.modelDraws > 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass with moving model matrices, "
                                          << counts.modelDraws << " draws of " << counts.models << " of them ("
                                          << counts.modelsStill << " still, " << counts.modelsUnseen
                                          << " without a last frame, " << sRegionMatrices.size()
                                          << " regions' own passed over)" << LL_ENDL;
            }
        }
        if (counts.reactiveDraws > 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass with reactive draws, "
                                          << counts.reactiveDraws << " draws (" << counts.impostorsReactive
                                          << " quads of impostors made again in this frame, the others over "
                                          << counts.reactiveRanges
                                          << " vertex ranges of the flexible prims reshaped in this frame), strength "
                                          << strength << "; its stage of the reactive mask "
                                          << (reactive_staged ? "drawn" : "not drawn") << LL_ENDL;
            }
        }
        if (counts.impostorDraws > 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass with impostors, "
                                          << counts.impostorDraws << " quads (" << counts.impostorsReactive
                                          << " of impostors made again in this frame, reactive) of "
                                          << counts.impostorsMoved + counts.impostorsStill + counts.impostorsUnseen
                                          << " impostors drawn in this view (" << counts.impostorsMoved << " moved, "
                                          << counts.impostorsStill << " still, " << counts.impostorsUnseen
                                          << " without a last frame), " << sRefreshed.size()
                                          << " impostors made again in this frame" << LL_ENDL;
            }
        }
        if (counts.textureSmoothDraws + counts.textureFrameDraws > 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass with texture animations, "
                                          << counts.textureSmoothDraws
                                          << " draws of smooth steps with their own motion, "
                                          << counts.textureFrameDraws
                                          << " draws of frame steps (and cut-out or alpha smooth ones) marked "
                                          << "reactive, of "
                                          << counts.textureSmooth << " smooth steps, " << counts.textureFrame
                                          << " frame steps and " << counts.textureMade
                                          << " texture matrices made in this frame's object update; "
                                          << gLuminousTextureMotionProgram.mName << " "
                                          << (texture_ready ? "ready" : "not ready") << LL_ENDL;
            }
        }
        if (counts.avatarDraws > 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                LL_INFOS("LuminousMetal") << "MetalFX object motion: first pass with system avatar parts, "
                                          << counts.avatarDraws << " draws of " << counts.avatarParts
                                          << " moving skinned parts (" << counts.avatarStill << " still, "
                                          << counts.avatarUnseen << " without a last frame)" << LL_ENDL;
            }
        }
    }

    LL::RHI::Texture* texture()
    {
        return sTarget.getRHITexture(0);
    }

    void beginFrame()
    {
        sReshaped.clear();
        sRefreshed.clear();
    }

    void reshaped(LLDrawable* drawable)
    {
        if (drawable && LuminousMetal::objectMotionOn())
        {
            sReshaped.push_back(drawable);
        }
    }

    void impostorRefreshed(const LLVOAvatar* avatar)
    {
        if (avatar && LuminousMetal::objectMotionOn())
        {
            sRefreshed.emplace_back(avatar, avatar->getID());
        }
    }

    void beginTextureSteps()
    {
        sTextureSteps.clear();
        sTextureStepOrder.clear();
        sTextureStepsSorted = true;
        sTextureStepCounts = TextureStepCounts();
    }

    bool textureStepSmooth(const LLViewerTextureAnim& anim, F32 frame_before)
    {
        if (!(anim.mMode & LLTextureAnim::SMOOTH))
        {
            return false;
        }
        const F32 frame = anim.getLastFrame();
        const bool rotate = anim.mMode & LLTextureAnim::ROTATE;
        const bool translate = !(anim.mMode & (LLTextureAnim::ROTATE | LLTextureAnim::SCALE));
        const bool grid = translate && anim.mSizeX && anim.mSizeY;
        const bool one_row = grid && anim.mSizeY == 1;
        F32 moved = frame - frame_before;
        if (translate && !grid)
        {
            // the counter is the offset in s itself: a move by whole repeats leaves the texture as it was
            moved -= std::round(moved);
        }
        else if (one_row)
        {
            // the offset in s is the counter over mSizeX apart from whole repeats, and the offset in t moves by a whole
            // repeat at each row: a move by whole rows (mSizeX cells) leaves the texture as it was
            const F32 rows = moved / (F32)anim.mSizeX;
            moved = (rows - std::round(rows)) * (F32)anim.mSizeX;
        }
        else if (rotate)
        {
            // the counter is the angle: a move by whole turns leaves the texture as it was
            moved -= F_TWO_PI * std::round(moved / F_TWO_PI);
        }
        const F32 most = TEXTURE_STEP_MOST * std::fabs(anim.mRate) * gFrameIntervalSeconds.value();
        if (!(std::fabs(moved) <= most))
        {
            return false;
        }
        // a grid's TRANSLATE of more rows steps its offset in t by a whole cell at the end of each row, as
        // animateTextures gives it
        return !grid || one_row || (S32)(frame / anim.mSizeX) == (S32)(frame_before / anim.mSizeX);
    }

    void textureStepped(const LLMatrix4* matrix, bool made, bool smooth)
    {
        if (!matrix || !LuminousMetal::objectMotionOn())
        {
            return;
        }
        TextureStep& step = sTextureSteps.emplace_back();
        step.matrix = matrix;
        step.last = *matrix;
        step.smooth = smooth;
        step.made = made;
        sTextureStepsSorted = false;
    }

    void release()
    {
        sTarget.release();
        sModels.clear();
        sAvatarRoots.clear();
        sAvatarPalettes.clear();
        sAvatarParts.clear();
        sReshaped.clear();
        sReactiveRanges.clear();
        sRefreshed.clear();
        std::vector<TextureStep>().swap(sTextureSteps);
        std::vector<U32>().swap(sTextureStepOrder);
        sTextureStepsSorted = true;
        sTextureStepCounts = TextureStepCounts();
    }
}
