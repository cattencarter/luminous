// A plain copy of a texture into the target: the fullscreen triangle and a linear sample at each
// fragment's uv. Used by the tests and wherever a native pass needs a straight copy.

#include "fullscreen.h"

fragment float4 luminous_copy_fs(LuminousFullscreenOut in [[stage_in]], texture2d<float> source [[texture(0)]])
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    return source.sample(linearClamp, in.uv);
}
