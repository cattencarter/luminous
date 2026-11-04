/**
 * @file llcubemaparray.cpp
 * @brief LLCubeMap class implementation
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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
#include "linden_common.h"

#include "llworkerthread.h"

#include "llcubemaparray.h"

#include "v4coloru.h"
#include "v3math.h"
#include "v3dmath.h"
#include "m3math.h"
#include "m4math.h"

#include "llrender.h"
#include "llglslshader.h"
#include "llrhi.h"
#include "llrhigl.h" // [Viewer Lab] GLNames

#include "llglheaders.h"

#include <atomic>

//#pragma optimize("", off)

using namespace LLImageGLMemory;

// [Viewer Lab] LLCubeMapArray's GL backend functions below do nothing when GL is not the backend chosen at startup,
// as llimagegl.cpp's gl_is_backend.
static bool gl_is_backend()
{
    return LL::RHI::viewerBackend() == LL::RHI::BackendType::GL;
}

// [Viewer Lab] allocate's glTexImage3D of one level of the cube map array without data, on the CubeArray texture
// LLImageGL::createOwnTexture made; nothing when GL is not the backend
static void gl_tex_image_3d(U32 level, U32 format, U32 size, U32 depth)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llcubemaparray:gl_tex_image_3d");
        return;
    }
    glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, level, format, size, size, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

// [Viewer Lab] The copy constructor's glGetTexImage of what GL has bound, with the caller's GL format and type: texture 0,
// because allocate ends with unbind, so nothing is read (measured, LLRHI GL transfers test<5>); nothing when GL is not
// the backend
static void gl_get_tex_image(U32 target, S32 level, U32 format, U32 type, void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llcubemaparray:gl_get_tex_image");
        return;
    }
    glGetTexImage(target, level, format, type, data);
}

// [Viewer Lab] The copy constructor's glTexSubImage3D of each face into what GL has bound: texture 0, so GL refuses it
// (measured, LLRHI GL transfers test<5>); nothing when GL is not the backend
static void gl_tex_sub_image_3d(U32 target, S32 level, S32 x, S32 y, S32 z, S32 width, S32 height, S32 depth, U32 format,
                                U32 type, const void* data)
{
    if (!gl_is_backend())
    {
        LL_GL_GATED("llcubemaparray:gl_tex_sub_image_3d");
        return;
    }
    glTexSubImage3D(target, level, x, y, z, width, height, depth, format, type, data);
}

// MUST match order of OpenGL face-layers
GLenum LLCubeMapArray::sTargets[6] =
{
    GL_TEXTURE_CUBE_MAP_POSITIVE_X,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
    GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
    GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
};

LLVector3 LLCubeMapArray::sLookVecs[6] =
{
        LLVector3(1, 0, 0),
        LLVector3(-1, 0, 0),
        LLVector3(0, 1, 0),
        LLVector3(0, -1, 0),
        LLVector3(0, 0, 1),
        LLVector3(0, 0, -1)
};

LLVector3 LLCubeMapArray::sUpVecs[6] =
{
    LLVector3(0, -1, 0),
    LLVector3(0, -1, 0),
    LLVector3(0, 0, 1),
    LLVector3(0, 0, -1),
    LLVector3(0, -1, 0),
    LLVector3(0, -1, 0)
};

LLVector3 LLCubeMapArray::sClipToCubeLookVecs[6] =
{
        LLVector3(0, 0, -1), //GOOD
        LLVector3(0, 0, 1), //GOOD

        LLVector3(1, 0, 0), // GOOD
        LLVector3(1, 0, 0), // GOOD

        LLVector3(1, 0, 0),
        LLVector3(-1, 0, 0),
};

LLVector3 LLCubeMapArray::sClipToCubeUpVecs[6] =
{
    LLVector3(-1, 0, 0), //GOOD
    LLVector3(1, 0, 0), //GOOD

    LLVector3(0, 1, 0), // GOOD
    LLVector3(0, -1, 0), // GOOD

    LLVector3(0, 0, -1),
    LLVector3(0, 0, 1)
};

LLCubeMapArray::LLCubeMapArray()
    : mTextureStage(0)
{

}

LLCubeMapArray::LLCubeMapArray(LLCubeMapArray& lhs, U32 width, U32 count) : mTextureStage(0)
{
    mWidth = width;
    mCount = count;

    // Allocate a new cubemap array with the same criteria as the incoming cubemap array
    allocate(mWidth, lhs.mImage->getComponents(), count, lhs.mImage->getUseMipMaps(), lhs.mHDR);

    // [Viewer Lab] Coverage marker, once per session
    static std::atomic<bool> sReached{ false };
    if (!sReached.exchange(true))
    {
        LL_INFOS("RHI") << "Transfer site reached: LLCubeMapArray copy" << LL_ENDL;
    }

    // Copy each cubemap from the incoming array to the new array
    U32 min_count = std::min(count, lhs.mCount);
    for (U32 i = 0; i < min_count * 6; ++i)
    {
        U32 src_resolution = lhs.mWidth;
        U32 dst_resolution = mWidth;
        {
            GLint components = GL_RGB;
            if (mImage->getComponents() == 4)
                components = GL_RGBA;
            // GLint format = GL_RGB; // <FS:Beq/> unused

            // Handle different resolutions by scaling the image
            LLPointer<LLImageRaw> src_image = new LLImageRaw(lhs.mWidth, lhs.mWidth, lhs.mImage->getComponents());
            gl_get_tex_image(GL_TEXTURE_CUBE_MAP_ARRAY, 0, components, GL_UNSIGNED_BYTE, src_image->getData()); // [Viewer Lab]

            LLPointer<LLImageRaw> scaled_image = src_image->scaled(mWidth, mWidth);
            gl_tex_sub_image_3d(GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, i, mWidth, mWidth, 1, components, GL_UNSIGNED_BYTE, scaled_image->getData()); // [Viewer Lab]
        }
    }
}

LLCubeMapArray::~LLCubeMapArray()
{
}

void LLCubeMapArray::allocate(U32 resolution, U32 components, U32 count, bool use_mips, bool hdr)
{
    mWidth = resolution;
    mCount = count;

    mHDR = hdr;

    // [Viewer Lab] The format before the image, for its description
    U32 format = components == 4 ? GL_RGBA16F : GL_R11F_G11F_B10F;
    if (!hdr)
    {
        format = components == 4 ? GL_RGBA8 : GL_RGB8;
    }

    mImage = new LLImageGL(resolution, resolution, components, use_mips);
    // [Viewer Lab] A CubeArray texture of the viewer's render device through LLImageGL, on today's one name from its
    // pool; glTexImage3D gets the format uncompressed
    mImage->createOwnTexture(LLTexUnit::TT_CUBE_MAP_ARRAY, format, GL_RGBA, GL_UNSIGNED_BYTE, LL::RHI::TEXTURE_SAMPLED,
                             use_mips ? LL::RHI::fullMipCount(resolution, resolution) : 1, count, false);
    mImage->setTarget(sTargets[0], LLTexUnit::TT_CUBE_MAP_ARRAY);

    mImage->setUseMipMaps(use_mips);
    mImage->setHasMipMaps(use_mips);

    bind(0);
    free_cur_tex_image();

    U32 mip = 0;
    U32 mip_resolution = resolution;
    while (mip_resolution >= 1)
    {
        gl_tex_image_3d(mip, format, mip_resolution, count * 6); // [Viewer Lab]

        if (!use_mips)
        {
            break;
        }
        mip_resolution /= 2;
        ++mip;
    }

    alloc_tex_image(resolution, resolution, format, count * 6);

    mImage->setAddressMode(LLTexUnit::TAM_CLAMP);

    if (use_mips)
    {
        mImage->setFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
        //glGenerateMipmap(GL_TEXTURE_CUBE_MAP_ARRAY);  // <=== latest AMD drivers do not appreciate this method of allocating mipmaps
    }
    else
    {
        mImage->setFilteringOption(LLTexUnit::TFO_BILINEAR);
    }

    unbind();
}

// [Viewer Lab] The GL backend's raw-name bind for an array without a texture of the device (an empty array keeps its
// raw name)
static bool bind_raw(LLTexUnit* unit, U32 name, bool has_mips)
{
    return gl_bind_raw_name(unit, LLTexUnit::TT_CUBE_MAP_ARRAY, name, has_mips);
}

void LLCubeMapArray::bind(S32 stage)
{
    mTextureStage = stage;
    // [Viewer Lab] bindManual's GL by the array's device texture, or its raw name
    if (LL::RHI::Texture* texture = mImage->getRHITexture())
    {
        gGL.getTexUnit(stage)->bindTexture(LLTexUnit::TT_CUBE_MAP_ARRAY, texture, mImage->getUseMipMaps());
    }
    else
    {
        bind_raw(gGL.getTexUnit(stage), getGLName(), mImage->getUseMipMaps());
    }
}

void LLCubeMapArray::unbind()
{
    gGL.getTexUnit(mTextureStage)->unbind(LLTexUnit::TT_CUBE_MAP_ARRAY);
    mTextureStage = -1;
}

GLuint LLCubeMapArray::getGLName()
{
    return LL::RHI::GLNames::of(mImage.get()); // [Viewer Lab]
}

void LLCubeMapArray::destroyGL()
{
    mImage = NULL;
}
