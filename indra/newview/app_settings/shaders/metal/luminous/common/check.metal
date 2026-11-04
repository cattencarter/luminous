// Uses every common helper once, so the offline check compiles each of them (a header alone is never
// compiled until a library includes it).

#include "colour.h"
#include "filters.h"
#include "fullscreen.h"
#include "vogel.h"

fragment float4 luminous_common_check_fs(LuminousFullscreenOut in [[stage_in]], texture2d<float> source [[texture(0)]])
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    const float2 texel = 1.0 / float2(source.get_width(), source.get_height());
    float4 c = luminousTent9(source, linearClamp, in.uv, texel, 1.0);
    c += luminousJimenez13(source, linearClamp, in.uv + luminousVogel(3, 16, 0.5) * texel, texel);
    c.rgb = luminousLinearToSRGB(luminousSRGBToLinear(c.rgb)) * luminousLuminance(c.rgb);
    return c;
}
