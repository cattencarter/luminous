// MetalFX temporal's inputs, drawn at the live view's size, and the present of the frame it scaled.
// - Motion: per texel, where its point was in the last frame minus where it is now without the jitter, in texels of
//   the viewer's row order (row 0 is GL's bottom row), from the depth and the camera's matrices: the camera's
//   motion only.
// - Reactive mask: how much of a texel's light the alpha, particles and water drawn since the last copy of the frame
//   changed, relative to its light, scaled, faded with distance and, when asked, only where the depth did not change;
//   1 tells MetalFX not to trust its history there. Drawn in stages, each one the largest of its own and the stage
//   before.
// - Present: the scaled frame into the back buffer texel for texel, film grain on it at the window's size, then the
//   present's noise, clamp and depth (present.metal). What a frame drawn for the scaler leaves out because it is
//   fixed to the screen is drawn here on the scaled frame (LuminousTemporalScreenConstants): live lens distortion
//   reads it through the lens's mapping, Glitch reads it where its pattern says (common/glitch.h), and the light
//   leak, grain and Worn photo follow in the tonemap's order (wornleak.h). A frame drawn for the scaler that the
//   scaler gave no frame for is scaled with Catmull-Rom and given the same.
// - Hold: per block of 32 x 32 texels, the largest difference between this frame and the one the hold began with.

#include "luminous/common/distortion.h"
#include "luminous/common/fullscreen.h"
#include "luminous/common/glitch.h"
#include "luminous/tone/toneshared.h"
#include "luminous/tone/wornleak.h"

namespace luminous_temporal
{
inline float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// The share of a texel's light that changed between before and now: 0 unchanged, 1 at a change as large as the light.
inline float reactive(float3 now, float3 before)
{
    now = clamp(select(now, float3(0.0), isnan(now)), 0.0, 64.0);
    before = clamp(select(before, float3(0.0), isnan(before)), 0.0, 64.0);
    const float change = luma(abs(now - before));
    return saturate(4.0 * change / (luma(now) + luma(before) + 0.01));
}

// The share above, scaled and placed: only where the depth did not change when asked (blended surfaces write no
// depth; cut-outs and opaque fullbright do, and their motion is their own), faded out with distance.
inline float placed(float share, uint2 texel, constant LuminousTemporalMaskConstants& k, depth2d<float> depthNow,
                    depth2d<float> depthBefore)
{
    const float depth = depthNow.read(texel);
    if (k.blendedOnly > 0.5 && abs(depth - depthBefore.read(texel)) > 1e-7)
    {
        return 0.0;
    }
    float fade = 1.0;
    if (k.fadeEnd > 0.0)
    {
        const float distance = k.projB / (depth * 2.0 - 1.0 + k.projA);
        fade = 1.0 - smoothstep(k.fadeStart, k.fadeEnd, distance);
    }
    return share * k.strength * fade;
}

inline float3 toLinear(float3 c)
{
    return select(pow((c + 0.055) / 1.055, float3(2.4)), c / 12.92, c <= 0.04045);
}

inline float3 toDisplay(float3 c)
{
    return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
}

// Photo mode's film grain, as CASF.glsl's luminousGrain draws it, in linear light on cells of the window's pixels.
inline float grainHash(float2 p)
{
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

inline float3 grain(float3 c, float2 window, constant LuminousTemporalConstants& k)
{
    if (k.grain <= 0.0)
    {
        return c;
    }
    const float2 cell = window / max(k.grainSize * k.outputHeight / 1080.0, 1.0);
    const float2 i = floor(cell);
    const float2 f = smoothstep(0.0, 1.0, fract(cell));
    const float n = mix(mix(grainHash(i), grainHash(i + float2(1.0, 0.0)), f.x),
                        mix(grainHash(i + float2(0.0, 1.0)), grainHash(i + float2(1.0, 1.0)), f.x), f.y);
    const float l = sqrt(clamp(luma(c), 0.0, 1.0));
    return c * (1.0 + (n - 0.5) * k.grain * 0.6 * (4.0 * l * (1.0 - l)));
}

// Where live lens distortion reads the frame for the world view's uv.
inline float2 distorted(float2 uv, constant LuminousTemporalScreenConstants& s)
{
    return luminousDistorted(uv, s.outputSize.x / max(s.outputSize.y, 1.0), s.frameScale, s.distortion,
                             s.pictureAspect);
}

// The scaled frame at uv: texel for texel, or, with live lens distortion, read bilinearly where it moves the picture.
inline float4 scaledAt(texture2d<float> frame, float2 uv, constant LuminousTemporalScreenConstants& s)
{
    if (s.distortion != 0.0)
    {
        constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
        return frame.sample(linearClamp, distorted(uv, s), level(0.0));
    }
    const int2 size = int2(frame.get_width(), frame.get_height());
    const int2 texel = clamp(int2(uv * float2(size)), int2(0), size - 1);
    return frame.read(uint2(texel));
}

// The picture Glitch reads at uv, in linear light: the scaled frame through live lens distortion, as the lens's passes
// read it, bilinearly, or with Catmull-Rom for a frame the scaler gave no frame for. Glitch draws on the HDR frame
// before the tonemap; here its colours are mixed in the display's linear light, which measured closer to that than
// light lifted back toward the scene's.
inline float3 glitchRead(texture2d<float> frame, float2 uv, constant LuminousTemporalScreenConstants& s, bool catmull)
{
    constexpr sampler linearClamp(filter::linear, address::clamp_to_edge);
    const float2 at = s.distortion != 0.0 ? distorted(uv, s) : uv;
    const float3 c = catmull ? luminous_present::catmullRom(frame, at).rgb : frame.sample(linearClamp, at, level(0.0)).rgb;
    return toLinear(saturate(c));
}

// Glitch on the scaled frame at the world view's uv, display-referred; the alpha is the pixel's own.
inline float4 glitched(texture2d<float> frame, float2 uv, float alpha, constant LuminousTemporalScreenConstants& s,
                       bool catmull)
{
    const LuminousGlitch g = luminousGlitchAt(uv, s.outputSize.x / max(s.outputSize.y, 1.0), s.outputSize.y,
                                              s.frameScale > 0.0 ? s.frameScale : 1.0, s.pictureAspect, s.glitch,
                                              s.glitchParts, s.glitchSeed);
    float3 colour;
    colour.r = glitchRead(frame, luminousGlitchUV(g, g.at + g.offset), s, catmull).r;
    colour.gb = glitchRead(frame, luminousGlitchUV(g, g.at - g.offset), s, catmull).gb;
    if (g.vhs > 0.0)
    {
        const float3 left = glitchRead(frame, luminousGlitchSideUV(g, -1.0), s, catmull);
        const float3 right = glitchRead(frame, luminousGlitchSideUV(g, 1.0), s, catmull);
        float3 chroma = float3(0.0);
        for (int k = 1; k <= 4; ++k)
        {
            chroma += luminousGlitchChroma(glitchRead(frame, luminousGlitchBehindUV(g, k), s, catmull));
        }
        colour = luminousGlitchTape(g, colour, left, right, chroma);
    }
    colour = luminousGlitchLines(g, colour, s.glitchLineSize);
    return float4(toDisplay(clamp(colour, 0.0, 1.0)), alpha);
}

// What is drawn on the finished, display-referred colour of the scaled frame, in linear light as the tonemap's last
// steps: the light leak, film grain, then Worn photo. uv is the world view's, window the fragment's place in the
// window.
inline float4 afterUpscale(float4 c, float2 uv, float2 window, constant LuminousTemporalConstants& k,
                           constant LuminousTemporalScreenConstants& s)
{
    if (s.leak > 0.0 || s.worn > 0.0)
    {
        float3 l = toLinear(saturate(c.rgb));
        l = luminous_screen::lightLeak(l, uv, s);
        l = clamp(grain(l, window, k), 0.0, 1.0);
        l = luminous_screen::worn(l, uv, s);
        c.rgb = toDisplay(clamp(l, 0.0, 1.0));
    }
    else if (k.grain > 0.0)
    {
        c.rgb = toDisplay(clamp(grain(toLinear(saturate(c.rgb)), window, k), 0.0, 1.0));
    }
    return c;
}

// The scaled frame, what was left out of it, then the present's noise and clamp.
inline float4 scaled(texture2d<float> frame, float2 uv, float2 window, constant LuminousTemporalConstants& k,
                     constant LuminousTemporalScreenConstants& s)
{
    float4 c = scaledAt(frame, uv, s);
    if (s.glitch > 0.0)
    {
        c = glitched(frame, uv, c.a, s, false);
    }
    return luminous_present::finish(afterUpscale(c, uv, window, k, s), window);
}

// A frame drawn for the scaler that the scaler gave no frame for, scaled with Catmull-Rom (through live lens
// distortion), what was left out of it, then the present's noise and clamp.
inline float4 screenScaled(texture2d<float> frame, float2 uv, float2 window, constant LuminousTemporalConstants& k,
                           constant LuminousTemporalScreenConstants& s)
{
    const float2 at = s.distortion != 0.0 ? distorted(uv, s) : uv;
    float4 c = luminous_present::catmullRom(frame, at);
    if (s.glitch > 0.0)
    {
        c = glitched(frame, uv, c.a, s, true);
    }
    return luminous_present::finish(afterUpscale(c, uv, window, k, s), window);
}
}

fragment float2 luminous_temporal_motion_fs(LuminousFullscreenOut in [[stage_in]],
                                            constant LuminousTemporalConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                            depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    const float2 p = in.position.xy; // the texel's centre, GL's rows
    const float depth = depthMap.read(uint2(p));
    const float4 clip = float4(p / k.size * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    const float4 last = k.clipToLastClip * clip;
    const float2 lastTexel = (last.xy / max(abs(last.w), 1e-6) * sign(last.w) * 0.5 + 0.5) * k.size;
    return (lastTexel - (p - k.jitter)) * k.motionSign;
}

fragment float luminous_temporal_reactive_first_fs(LuminousFullscreenOut in [[stage_in]],
                                                   constant LuminousTemporalMaskConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                   texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                   texture2d<float> before [[texture(LUMINOUS_TONE_TEXTURE_BEFORE)]],
                                                   depth2d<float> depthNow [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]],
                                                   depth2d<float> depthBefore [[texture(LUMINOUS_TONE_TEXTURE_BEFORE_DEPTH)]])
{
    const uint2 texel = uint2(in.position.xy);
    const float share = luminous_temporal::reactive(frame.read(texel).rgb, before.read(texel).rgb);
    return luminous_temporal::placed(share, texel, k, depthNow, depthBefore);
}

fragment float luminous_temporal_reactive_more_fs(LuminousFullscreenOut in [[stage_in]],
                                                  constant LuminousTemporalMaskConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                  texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                  texture2d<float> before [[texture(LUMINOUS_TONE_TEXTURE_BEFORE)]],
                                                  texture2d<float> mask [[texture(LUMINOUS_TONE_TEXTURE_MASK)]],
                                                  depth2d<float> depthNow [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]],
                                                  depth2d<float> depthBefore [[texture(LUMINOUS_TONE_TEXTURE_BEFORE_DEPTH)]])
{
    const uint2 texel = uint2(in.position.xy);
    const float share = luminous_temporal::reactive(frame.read(texel).rgb, before.read(texel).rgb);
    return max(mask.read(texel).r, luminous_temporal::placed(share, texel, k, depthNow, depthBefore));
}

// A blurred layer averaged over the frames: this frame's (now, the layer in its lower left part) with the last average
// (last), read where the camera saw the same place in the last frame; the place's depth read where the jittered camera
// sees it. This frame's is taken whole without a last average, outside the last frame, and more of it as the camera
// moved the place further or, with react, as the layer changed there (something moved in it). With react 2 that is
// told by the layer's weight (alpha) alone: the last average's weight lying outside what this frame's layer holds round
// the place (something in focus moved over the blur or off it); what the jitter changes at a focus edge lies within
// that range, so it stays averaged.
fragment float4 luminous_temporal_settle_fs(LuminousFullscreenOut in [[stage_in]],
                                            constant LuminousTemporalSettleConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                            texture2d<float> now [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                            texture2d<float> last [[texture(LUMINOUS_TONE_TEXTURE_BEFORE)]],
                                            depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    const uint2 texel = uint2(in.position.xy);
    float4 c = now.read(texel);
    // NaN to 0 and infinities to half float's largest, here and in the history: mix() below would make
    // Inf - Inf a NaN, which the average would then keep frame after frame
    c = clamp(select(c, float4(0.0), isnan(c)), -65504.0, 65504.0);
    if (k.reset > 0.5)
    {
        return c;
    }
    const float2 uv = in.position.xy / k.region;
    const float2 depthSize = float2(depthMap.get_width(), depthMap.get_height());
    const float depth = depthMap.read(uint2(clamp((uv + k.depthJitter) * depthSize, float2(0.0), depthSize - 1.0)));
    const float4 lastClip = k.toLast * float4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    if (lastClip.w <= 1e-6)
    {
        return c;
    }
    const float2 lastUV = lastClip.xy / lastClip.w * 0.5 + 0.5;
    if (any(lastUV < 0.0) || any(lastUV > 1.0))
    {
        return c;
    }
    const float2 lastTexel = min(lastUV * k.region, k.region - 0.5);
    float4 h = last.sample(linear, lastTexel / k.lastSize);
    h = clamp(select(h, c, isnan(h)), -65504.0, 65504.0);
    const float moved = length((lastUV - uv) * k.frameSize);
    float share = mix(k.share, 1.0, saturate(moved / max(k.motionFull, 1e-3)));
    if (k.react > 1.5)
    {
        float lo = c.a;
        float hi = c.a;
        const int2 edge = int2(k.region) - 1;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                const float a = now.read(uint2(clamp(int2(texel) + int2(x, y), int2(0), edge))).a;
                lo = min(lo, a);
                hi = max(hi, a);
            }
        }
        share = max(share, smoothstep(0.25, 0.6, max(max(h.a - hi, lo - h.a), 0.0)));
    }
    else if (k.react > 0.5)
    {
        const float a = luminous_temporal::luma(max(c.rgb, float3(0.0)));
        const float b = luminous_temporal::luma(max(h.rgb, float3(0.0)));
        share = max(share, smoothstep(0.25, 0.6, abs(a - b) / (max(a, b) + 0.05)));
    }
    return mix(h, c, share);
}

// A frame with no alpha stage: nothing is reactive.
fragment float luminous_temporal_reactive_none_fs(LuminousFullscreenOut in [[stage_in]])
{
    return 0.0;
}

// The depth is the frame's own at the undistorted place, as every other present writes it: the interface drawn over
// the world is placed undistorted too, and tests against it.
fragment LuminousPresentOut luminous_present_temporal_fs(LuminousFullscreenOut in [[stage_in]],
                                                         constant LuminousTemporalConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                         constant LuminousTemporalScreenConstants& s [[buffer(LUMINOUS_TONE_BUFFER_SCREEN)]],
                                                         texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                         depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    LuminousPresentOut out;
    out.colour = luminous_temporal::scaled(frame, in.uv, in.position.xy, k, s);
    out.depth = luminous_present::nearestDepth(depthMap, in.uv);
    return out;
}

// The same into a back buffer pass that has no depth to write.
fragment float4 luminous_present_temporal_nodepth_fs(LuminousFullscreenOut in [[stage_in]],
                                                     constant LuminousTemporalConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                     constant LuminousTemporalScreenConstants& s [[buffer(LUMINOUS_TONE_BUFFER_SCREEN)]],
                                                     texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]])
{
    return luminous_temporal::scaled(frame, in.uv, in.position.xy, k, s);
}

// The Catmull-Rom present of a frame drawn for the scaler that the scaler gave no frame for (it settles, or let go
// in this frame), with what that frame left out; the depth as above.
fragment LuminousPresentOut luminous_present_screen_fs(LuminousFullscreenOut in [[stage_in]],
                                                       constant LuminousTemporalConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                       constant LuminousTemporalScreenConstants& s [[buffer(LUMINOUS_TONE_BUFFER_SCREEN)]],
                                                       texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                                       depth2d<float> depthMap [[texture(LUMINOUS_TONE_TEXTURE_DEPTH)]])
{
    LuminousPresentOut out;
    out.colour = luminous_temporal::screenScaled(frame, in.uv, in.position.xy, k, s);
    out.depth = luminous_present::nearestDepth(depthMap, in.uv);
    return out;
}

// The same into a back buffer pass that has no depth to write.
fragment float4 luminous_present_screen_nodepth_fs(LuminousFullscreenOut in [[stage_in]],
                                                   constant LuminousTemporalConstants& k [[buffer(LUMINOUS_TONE_BUFFER_TEMPORAL)]],
                                                   constant LuminousTemporalScreenConstants& s [[buffer(LUMINOUS_TONE_BUFFER_SCREEN)]],
                                                   texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]])
{
    return luminous_temporal::screenScaled(frame, in.uv, in.position.xy, k, s);
}

// The largest difference of any channel between the frame and the reference within this texel's block of
// LUMINOUS_TONE_HOLD_BLOCK x LUMINOUS_TONE_HOLD_BLOCK texels of them.
fragment float luminous_temporal_hold_diff_fs(LuminousFullscreenOut in [[stage_in]],
                                              texture2d<float> frame [[texture(LUMINOUS_TONE_TEXTURE_FRAME)]],
                                              texture2d<float> reference [[texture(LUMINOUS_TONE_TEXTURE_BEFORE)]])
{
    const uint2 size = uint2(frame.get_width(), frame.get_height());
    const uint block = uint(LUMINOUS_TONE_HOLD_BLOCK);
    const uint2 base = uint2(in.position.xy) * block;
    const uint2 end = min(base + block, size);
    float most = 0.0;
    for (uint y = base.y; y < end.y; ++y)
    {
        for (uint x = base.x; x < end.x; ++x)
        {
            const float3 d = abs(frame.read(uint2(x, y)).rgb - reference.read(uint2(x, y)).rgb);
            most = max(most, max(d.r, max(d.g, d.b)));
        }
    }
    return most;
}
