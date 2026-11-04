// A render's frames averaged as light, and the frame read back as the picture.
//
// Averaging: each frame of a render of several, just before tonemapping, is added at 1 / frames into a
// float target (half float for an eight-bit picture, 32-bit float for TIFF and EXR). The sum is the pass's
// own attachment, read through the attachment (programmable blending), so 32-bit float sums need no
// fixed-function blending; the first frame's pass clears it. In the last frame one pass takes both
// attachments, the sum and the HDR frame: it adds the frame and writes the sum back into the frame's
// colour, its alpha kept, so the frame is tonemapped once, from the average.
//
// Reading: one compute pass crops the frame to the picture (the margin's cut, and the render's shape) and
// box-filters it to the picture's size, as the CPU's reduce() does it for OpenGL, into a texture of the
// picture's size that is read back: half float for OpenEXR, at the render's exposure, 32-bit float for
// the finished picture of a sixteen-bit TIFF.

#include "luminous/common/fullscreen.h"
#include "luminous/render/rendershared.h"

// Frames between the first and the last: the frame added to the sum.
fragment float4 luminous_average_add_fs(LuminousFullscreenOut in [[stage_in]],
                                        float4 sum [[color(0)]],
                                        constant LuminousRenderParams& params [[buffer(LUMINOUS_RENDER_BUFFER_PARAMS)]],
                                        texture2d<float, access::read> frame [[texture(LUMINOUS_RENDER_TEXTURE_FRAME)]])
{
    return sum + frame.read(uint2(in.position.xy)) * params.weight;
}

struct LuminousAverageLast
{
    float4 sum [[color(0)]];
    float4 frame [[color(1)]];
};

// The last frame: added, and the sum written into the frame's colour (the pipeline writes colour only).
fragment LuminousAverageLast luminous_average_last_fs(LuminousFullscreenOut in [[stage_in]],
                                                      float4 sum [[color(0)]],
                                                      float4 frame [[color(1)]],
                                                      constant LuminousRenderParams& params [[buffer(LUMINOUS_RENDER_BUFFER_PARAMS)]])
{
    LuminousAverageLast out;
    out.sum = sum + frame * params.weight;
    out.frame = float4(out.sum.rgb, frame.a);
    return out;
}

// Whether a value is a finite number, by its bits: the library is built with fast math, which may take
// isfinite() as always true.
static inline bool3 luminous_finite(float3 v)
{
    return (as_type<uint3>(v) & 0x7f800000u) != 0x7f800000u;
}

// One picture pixel: its box of frame pixels inside the cut, chosen as reduce() chooses it.
kernel void luminous_render_resolve(constant LuminousRenderResolve& r [[buffer(LUMINOUS_RENDER_BUFFER_PARAMS)]],
                                    texture2d<float, access::read> source [[texture(LUMINOUS_RENDER_TEXTURE_SOURCE)]],
                                    texture2d<float, access::read> exposure [[texture(LUMINOUS_RENDER_TEXTURE_EXPOSURE)]],
                                    texture2d<float, access::write> picture [[texture(LUMINOUS_RENDER_TEXTURE_PICTURE)]],
                                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= r.pictureSize.x || gid.y >= r.pictureSize.y)
    {
        return;
    }
    const float frame_w = float(r.cutSize.x);
    const float frame_h = float(r.cutSize.y);
    const float ratio = min(frame_w / r.renderSize.x, frame_h / r.renderSize.y);
    const float crop_w = ratio * r.renderSize.x;
    const float crop_h = ratio * r.renderSize.y;
    const float x0 = (frame_w - crop_w) * 0.5;
    const float y0 = (frame_h - crop_h) * 0.5;
    const float sx = crop_w / float(r.pictureSize.x);
    const float sy = crop_h / float(r.pictureSize.y);

    // gid.y counts from the top; the frame's rows from the bottom.
    const float top = y0 + crop_h - float(gid.y) * sy;
    const float bottom = top - sy;
    int j0 = int(ceil(bottom - 0.5));
    int j1 = int(ceil(top - 0.5)) - 1;
    if (j1 < j0)
    {
        j0 = j1 = int(floor((top + bottom) * 0.5));
    }
    j0 = clamp(j0, 0, int(r.cutSize.y) - 1);
    j1 = clamp(j1, 0, int(r.cutSize.y) - 1);

    const float left = x0 + float(gid.x) * sx;
    const float right = left + sx;
    int i0 = int(ceil(left - 0.5));
    int i1 = int(ceil(right - 0.5)) - 1;
    if (i1 < i0)
    {
        i0 = i1 = int(floor((left + right) * 0.5));
    }
    i0 = clamp(i0, 0, int(r.cutSize.x) - 1);
    i1 = clamp(i1, 0, int(r.cutSize.x) - 1);

    float3 sum = float3(0.0);
    for (int j = j0; j <= j1; ++j)
    {
        for (int i = i0; i <= i1; ++i)
        {
            const float3 v = source.read(uint2(r.cutOrigin.x + uint(i), r.cutOrigin.y + uint(j))).rgb;
            sum += select(float3(0.0), v, luminous_finite(v));
        }
    }
    float3 c = sum / float((j1 - j0 + 1) * (i1 - i0 + 1));
    c *= r.scale;
    if (r.exposure != 0u)
    {
        c *= exposure.read(uint2(0, 0)).r;
    }
    if (r.halfRange != 0u)
    {
        c = select(float3(0.0), c, luminous_finite(c));
        c = clamp(c, float3(0.0), float3(65504.0));
    }
    picture.write(float4(c, 1.0), gid);
}
