// What a frame drawn for MetalFX's temporal scaler leaves out because it is fixed to the screen, drawn over the
// window's world view after the present, at the window's size: Firestorm's capture frame for a snapshot floater,
// then the picture frame of a shape chosen on the Render tab. Each looks as the pass that draws it into a frame
// (snapshotFrameF.glsl, frame.metal), measured in the window's pixels. Alpha blended over what the present drew, the
// alpha channel kept (colour mask), and no depth written: the interface drawn after reads the present's.

#include "luminous/common/fullscreen.h"
#include "luminous/tone/toneshared.h"

namespace luminous_overlay
{
// The fragment's place in the world view's pixels, as gl_FragCoord is in a frame of the world view's size.
inline float2 place(LuminousFullscreenOut in, constant LuminousOverlayConstants& k)
{
    return in.position.xy - k.origin;
}

// A texel index read through a mirrored repeat, as the post-processing targets' sampler reads one past an edge.
inline int mirrored(int i, int last)
{
    i = i < 0 ? -i - 1 : i;
    return i > last ? 2 * last + 1 - i : i;
}
}

// Firestorm's capture frame: outside its border grey, from a box of nine texels two pixels apart; its border band
// half the border's colour; inside unchanged. presented is the world view as the present drew it.
fragment float4 luminous_overlay_capture_frame_fs(LuminousFullscreenOut in [[stage_in]],
                                                  constant LuminousOverlayConstants& k [[buffer(LUMINOUS_TONE_BUFFER_OVERLAY)]],
                                                  texture2d<float> presented [[texture(LUMINOUS_TONE_TEXTURE_OVERLAY)]])
{
    const float2 px = luminous_overlay::place(in, k);
    const float4 rect = k.captureRect;
    const float4 border = float4(rect.xy - k.captureBorder.w, rect.zw + k.captureBorder.w);
    if (any(px < border.xy) || any(px > border.zw))
    {
        const int2 last = int2(presented.get_width(), presented.get_height()) - 1;
        const int2 texel = int2(floor(px));
        float3 blur = float3(0.0);
        for (int x = -1; x <= 1; ++x)
        {
            for (int y = -1; y <= 1; ++y)
            {
                const int2 at = texel + int2(x, y) * 2;
                blur += presented.read(uint2(luminous_overlay::mirrored(at.x, last.x),
                                             luminous_overlay::mirrored(at.y, last.y))).rgb;
            }
        }
        blur /= 9.0;
        const float grey = dot(blur, float3(0.299, 0.587, 0.114));
        return float4(float3(grey), 1.0);
    }
    if (any(px < rect.xy) || any(px > rect.zw))
    {
        return float4(k.captureBorder.xyz, 0.5);
    }
    return float4(0.0);
}

// The picture frame: outside the picture the view dimmed to 0.35, a line a pixel and a half wide just inside its
// edge, mixed 0.6 toward light grey.
fragment float4 luminous_overlay_picture_frame_fs(LuminousFullscreenOut in [[stage_in]],
                                                  constant LuminousOverlayConstants& k [[buffer(LUMINOUS_TONE_BUFFER_OVERLAY)]])
{
    const float2 px = luminous_overlay::place(in, k);
    const float4 rect = k.pictureRect;
    if (!(all(px >= rect.xy) && all(px <= rect.zw)))
    {
        return float4(0.0, 0.0, 0.0, 0.65);
    }
    const float toEdge = min(min(px.x - rect.x, rect.z - px.x), min(px.y - rect.y, rect.w - px.y));
    if (toEdge < 1.5)
    {
        return float4(float3(0.85), 0.6);
    }
    return float4(0.0);
}
