/**
 * @file llrhigl.h
 * @brief The render hardware interface's GL backend
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
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

#ifndef LL_LLRHIGL_H
#define LL_LLRHIGL_H

#include "llrhi.h"

class LLCubeMap;
class LLCubeMapArray;
class LLGLSLShader;
class LLImageGL;
class LLImageGLHandle;
class LLTexUnit;

// The GL backend: thin objects over the GL code where it lies in llrender, so raw GL in code not
// ported yet behaves as before. It needs a current GL context and an initialized LLGLManager, as
// llrender does, and binds through LLTexUnit and LLVertexBuffer so their caches stay right. Native
// objects are deleted with the delays LLImageGL and LLVertexBuffer already use, so FrameLifecycle
// only counts frames here; objects let go of on another thread are deleted on the main thread at
// the next frame end. An interface pass is a scope for GL state: what it changes is put back when
// it ends. The stack's passes (RenderPassDesc::stack, LLRenderTarget's) are not: on GL they keep
// the viewer's GL state, with nothing saved, put back or set to defaults (llrhigl.cpp).

namespace LL::RHI
{

/// What the GL backend reports: LLGLManager's values and a few queries of its own.
Capabilities capabilitiesFromGL();

/// [Viewer Lab] GL names are the GL backend's state: the interface and the viewer know only the 64-bit id, and inside
/// llrender the facades' gated GL binds, attaches and draws by these names. Code that needs a name asks the GL backend
/// here: the facades in llrender, the GL backend's tests and level 1's GL scenes. Nothing outside llrender uses GL names.
namespace GLNames
{
/// The GL name of an object of the GL backend, and 0 for any other (an object of another backend or of a test's device,
/// or none). A facade that holds a device texture detects "off GL" by the 0 (has_no_gl_name, deviceTextureOf).
U32 of(const Buffer* buffer);
U32 of(const Texture* texture);
U32 of(const Program* program);

/// The GL name a facade keeps for the GL backend, 0 off GL: LLImageGL's texture, a cube map's or a cube map array's,
/// and LLGLSLShader's program. assign gives the shader a program name, as a facade adopts one.
U32 of(const LLImageGL* image);
U32 of(LLCubeMap* cube);
U32 of(LLCubeMapArray* array);
U32 of(const LLGLSLShader& shader);
void assign(LLGLSLShader& shader, U32 program);

/// The GL name an LLImageGL handle stands for (createGLTexture's tex_name, which its caller gives back to setSubImage,
/// syncToMainThread and syncTexName), 0 off GL; and the handle of a GL name, for the GL backend's tests of those paths
/// with a raw name.
U32 of(const LLImageGLHandle& handle);
LLImageGLHandle handle(U32 name);

/// A texture of the GL backend under a live GL name it does not own, with an id of its own, as LLTexUnit sees a name GL
/// handed out again: for the tests of LLTexUnit's skip by the GL name on every GL, where the reuse itself is up to the
/// driver. It makes no GL call and never deletes the name.
Ref<Texture> alias(const TextureDesc& desc, U32 name);

/// LLImageGL's raw names on GL, for the tests of the facades' raw-name paths: names from LLImageGL's pool (generate),
/// handed to its deletion lists (release), the level of the texture bound on the active unit specified as LLImageGL
/// specifies its own (specifyBound), and a raw name bound on a unit through LLTexUnit's cache (bind; type is an
/// LLTexUnit::eTextureType).
void generate(S32 count, U32* names);
void release(S32 count, const U32* names);
void specifyBound(U32 target, S32 level, S32 internalFormat, S32 width, S32 height, U32 format, U32 type,
                  const void* pixels, bool allowCompression = true);
bool bind(LLTexUnit* unit, U32 type, U32 name, bool hasMips = false);
} // namespace GLNames

} // namespace LL::RHI

#endif // LL_LLRHIGL_H
