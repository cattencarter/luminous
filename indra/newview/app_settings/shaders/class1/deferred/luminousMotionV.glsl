/**
 * @file luminousMotionV.glsl
 * @brief MetalFX temporal: where a vertex is now and where it was in the last frame.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on Second Life Viewer Source Code, Copyright (C) 2007, Linden Research, Inc.
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

// The vertex placed as the deferred programs place it (this frame's camera, with
// MetalFX's jitter), and in the clip spaces of this frame and the last without
// the jitter: luminousMotionF.glsl turns the two into the surface's own motion.
// Rigged (HAS_SKIN): skinned with this frame's palette and the last frame's.
// The system avatar (LUMINOUS_AVATAR_SKIN): skinned as avatarSkinV.glsl skins
// it, with this frame's joint matrices and the last frame's, both to the agent
// frame (without the view).
// Otherwise: through this frame's model matrix and the last frame's; with
// LUMINOUS_TEXTURE_MOTION also the texture coordinates through this frame's
// texture matrix and the last frame's, for the texture's own motion.

uniform mat4 modelview_matrix;
uniform mat4 projection_matrix;
uniform mat4 luminous_view_proj;      // this frame's view-projection without the jitter
uniform mat4 luminous_last_view_proj; // the last frame's, without its jitter

in vec3 position;

out vec4 vary_now;
out vec4 vary_last;

#ifdef HAS_SKIN
mat4 getObjectSkinnedTransform();

in vec4 weight4;

uniform mat3x4 lastMatrixPalette[MAX_JOINTS_PER_MESH_OBJECT];

// getObjectSkinnedTransform (objectSkinV.glsl) with the last frame's palette
mat4 luminousLastSkinnedTransform()
{
    vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    index = min(index, vec4(MAX_JOINTS_PER_MESH_OBJECT-1));
    index = max(index, vec4( 0.0));

    w *= 1.0/(w.x+w.y+w.z+w.w);

    int i1 = int(index.x);
    int i2 = int(index.y);
    int i3 = int(index.z);
    int i4 = int(index.w);

    mat3 mat = mat3(lastMatrixPalette[i1])*w.x;
         mat += mat3(lastMatrixPalette[i2])*w.y;
         mat += mat3(lastMatrixPalette[i3])*w.z;
         mat += mat3(lastMatrixPalette[i4])*w.w;

    vec3 trans = vec3(lastMatrixPalette[i1][0].w,lastMatrixPalette[i1][1].w,lastMatrixPalette[i1][2].w)*w.x;
         trans += vec3(lastMatrixPalette[i2][0].w,lastMatrixPalette[i2][1].w,lastMatrixPalette[i2][2].w)*w.y;
         trans += vec3(lastMatrixPalette[i3][0].w,lastMatrixPalette[i3][1].w,lastMatrixPalette[i3][2].w)*w.z;
         trans += vec3(lastMatrixPalette[i4][0].w,lastMatrixPalette[i4][1].w,lastMatrixPalette[i4][2].w)*w.w;

    mat4 ret;

    ret[0] = vec4(mat[0], 0);
    ret[1] = vec4(mat[1], 0);
    ret[2] = vec4(mat[2], 0);
    ret[3] = vec4(trans, 1.0);

    return ret;
}
#elif defined(LUMINOUS_AVATAR_SKIN)
in vec4 weight;

uniform vec4 matrixPalette[45];     // this frame's joint matrices, avatarSkinV.glsl's layout, to the agent frame
uniform vec4 lastMatrixPalette[45]; // the last frame's

// avatarSkinV.glsl's getSkinnedTransform applied to p: the rows of the joint
// and of the next one, mixed by the weight's fraction
vec4 luminousAvatarNow(vec4 p, int i, int next, float x)
{
    return vec4(dot(mix(matrixPalette[i], matrixPalette[next], x), p),
                dot(mix(matrixPalette[i + 15], matrixPalette[next + 15], x), p),
                dot(mix(matrixPalette[i + 30], matrixPalette[next + 30], x), p),
                1.0);
}

vec4 luminousAvatarLast(vec4 p, int i, int next, float x)
{
    return vec4(dot(mix(lastMatrixPalette[i], lastMatrixPalette[next], x), p),
                dot(mix(lastMatrixPalette[i + 15], lastMatrixPalette[next + 15], x), p),
                dot(mix(lastMatrixPalette[i + 30], lastMatrixPalette[next + 30], x), p),
                1.0);
}
#else
uniform mat4 modelview_projection_matrix;
uniform mat4 luminous_model;      // this frame's model matrix, to the agent frame
uniform mat4 luminous_last_model; // the last frame's
#endif

#ifdef LUMINOUS_TEXTURE_MOTION
in vec2 texcoord0;

uniform mat4 luminous_texture_now;  // this frame's texture matrix, as texture_matrix0 is given
uniform mat4 luminous_texture_last; // the last frame's

out vec2 vary_texture_now;
out vec2 vary_texture_last;
#endif

void main()
{
    vec4 p = vec4(position.xyz, 1.0);
#ifdef HAS_SKIN
    // as the deferred programs' rigged variants place it
    mat4 skin = getObjectSkinnedTransform();
    mat4 mat = modelview_matrix * skin;
    vec4 pos = mat * p;
    gl_Position = projection_matrix * pos;
    vec4 now = skin * p;
    vec4 last = luminousLastSkinnedTransform() * p;
#elif defined(LUMINOUS_AVATAR_SKIN)
    int i = clamp(int(floor(weight.x)), 0, 14);
    int next = min(i + 1, 14);
    float x = fract(weight.x);
    vec4 now = luminousAvatarNow(p, i, next, x);
    vec4 last = luminousAvatarLast(p, i, next, x);
    gl_Position = projection_matrix * (modelview_matrix * now);
#else
    gl_Position = modelview_projection_matrix * p;
    vec4 now = luminous_model * p;
    vec4 last = luminous_last_model * p;
#endif
    vary_now = luminous_view_proj * now;
    vary_last = luminous_last_view_proj * last;
#ifdef LUMINOUS_TEXTURE_MOTION
    vary_texture_now = (luminous_texture_now * vec4(texcoord0, 0.0, 1.0)).xy;
    vary_texture_last = (luminous_texture_last * vec4(texcoord0, 0.0, 1.0)).xy;
#endif
}
