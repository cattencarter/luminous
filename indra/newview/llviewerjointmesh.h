/**
 * @file llviewerjointmesh.h
 * @brief Declaration of LLViewerJointMesh class
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLVIEWERJOINTMESH_H
#define LL_LLVIEWERJOINTMESH_H

#include "llviewerjoint.h"
#include "llviewertexture.h"
#include "llavatarjointmesh.h"
#include "llpolymesh.h"
#include "v4color.h"

class LLDrawable;
class LLFace;
class LLCharacter;
class LLViewerTexLayerSet;
class LLVertexBuffer; // <Luminous/>

//-----------------------------------------------------------------------------
// class LLViewerJointMesh
//-----------------------------------------------------------------------------
class LLViewerJointMesh : public LLAvatarJointMesh, public LLViewerJoint
{
public:
    // Constructor
    LLViewerJointMesh();

    // Destructor
    virtual ~LLViewerJointMesh();

    // Render time method to upload batches of joint matrices
    void uploadJointMatrices();

    // <Luminous> MetalFX temporal's object motion (luminousobjectmotion.cpp)
    // The floats of the joint matrices uploadJointMatrices gives the avatar programs: 45 vec4,
    // LL_CHARACTER_MAX_JOINTS_PER_MESH to each of the three rows
    static constexpr U32 LUMINOUS_JOINT_FLOATS = LL_CHARACTER_MAX_JOINTS_PER_MESH * 3 * 4;
    struct LuminousMotionDraw
    {
        LLVertexBuffer* buffer = nullptr;
        U32 start = 0;
        U32 end = 0;
        U32 count = 0;
        U32 offset = 0;
        bool skinned = false; // placed by its joints (uploadJointMatrices), else by its own world matrix
    };
    // Whether drawShape draws this mesh (its own checks), and what it draws: its face's buffer and range
    bool luminousMotionDraw(LuminousMotionDraw& draw);
    // The joint matrices uploadJointMatrices gives the avatar programs, without the view: to the agent frame, in the
    // same layout (the rows of joint n at n, n + 15 and n + 30), zero past the mesh's joints
    void luminousJointMatrices(F32 matrices[LUMINOUS_JOINT_FLOATS]);
    // </Luminous>

    // overloaded from base class
    U32 drawShape( F32 pixelArea, bool first_pass, bool is_dummy ) override;

    // necessary because MS's compiler warns on function inheritance via dominance in the diamond inheritance here.
    // warns even though LLViewerJoint holds the only non virtual implementation.
    U32 render(F32 pixelArea, bool first_pass = true, bool is_dummy = false) override { return LLViewerJoint::render(pixelArea, first_pass, is_dummy); }

    void updateFaceSizes(U32 &num_vertices, U32& num_indices, F32 pixel_area) override;
    void updateFaceData(LLFace *face, F32 pixel_area, bool damp_wind = false, bool terse_update = false) override;
    bool updateLOD(F32 pixel_area, bool activate) override;
    void updateJointGeometry() override;
    void dump() override;

    bool isAnimatable() const override { return false; }
};

#endif // LL_LLVIEWERJOINTMESH_H
