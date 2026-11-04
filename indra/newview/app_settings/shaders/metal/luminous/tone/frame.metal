// The picture frame over the finished live view: what a render in a shape of its own keeps. Outside the
// picture the view is dimmed, not hidden, so what is near can still be seen; a thin light line runs along
// the picture's edge, just inside. One fullscreen pass into the next post-processing target, as the
// OpenGL pass (luminousFrameF.glsl) draws it.

#include "luminous/common/fullscreen.h"
#include "luminous/tone/toneshared.h"

fragment float4 luminous_picture_frame_fs(LuminousFullscreenOut in [[stage_in]],
                                          constant LuminousPictureFrameConstants& constants [[buffer(LUMINOUS_TONE_BUFFER_FRAME)]],
                                          texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]])
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    float4 c = frame.sample(linearClamp, in.uv);

    // The fragment's position is GL's gl_FragCoord (fullscreen.h), the rect is in the same pixels.
    const float2 px = in.position.xy;
    const float4 rect = constants.rect;
    const bool inside = all(px >= rect.xy) && all(px <= rect.zw);
    if (!inside)
    {
        c.rgb *= 0.35;
        return c;
    }
    // the edge: a line a pixel and a half wide, just inside
    const float toEdge = min(min(px.x - rect.x, rect.z - px.x), min(px.y - rect.y, rect.w - px.y));
    if (toEdge < 1.5)
    {
        c.rgb = mix(c.rgb, float3(0.85), 0.6);
    }
    return c;
}
