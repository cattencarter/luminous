// The fullscreen triangle every native render pass of photo mode draws: draw(Triangles, 0, 3)
// without a vertex buffer.
//
// Coordinates follow the viewer's convention: every target has GL's row order (memory row 0 is GL's
// bottom row), so the vertex function makes GL's clip position and then applies the correction the
// translated GLSL gets at the end of each vertex stage: z = (z + w) * 0.5 and y = -y. uv (0, 0) is
// then GL's bottom left, which is memory row 0, so sampling a target at uv reads the texel under the
// fragment, and a fragment's [[position]].xy equals GL's gl_FragCoord.xy.

#ifndef LUMINOUS_FULLSCREEN_H
#define LUMINOUS_FULLSCREEN_H

#include <metal_stdlib>
using namespace metal;

struct LuminousFullscreenOut
{
    float4 position [[position]];
    float2 uv; ///< 0 to 1 over the target, (0, 0) at GL's bottom left
};

vertex LuminousFullscreenOut luminous_fullscreen_vs(uint id [[vertex_id]])
{
    // (-1, -1), (3, -1), (-1, 3): one triangle over the whole target.
    const float2 gl = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    LuminousFullscreenOut out;
    out.uv = gl * 0.5 + 0.5;
    float4 position = float4(gl, 0.0, 1.0);
    position.z = (position.z + position.w) * 0.5;
    position.y = -position.y;
    out.position = position;
    return out;
}

#endif // LUMINOUS_FULLSCREEN_H
