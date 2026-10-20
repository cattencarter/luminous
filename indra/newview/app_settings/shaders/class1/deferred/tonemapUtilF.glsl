/**
 * @file postDeferredTonemap.glsl
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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

/*[EXTRA_CODE_HERE]*/

uniform sampler2D exposureMap;
uniform vec2 screen_res;
in vec2 vary_fragcoord;

//===============================================================
// tone mapping taken from Khronos sample implementation
//===============================================================

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
const mat3 ACESInputMat = mat3
(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);


// ODT_SAT => XYZ => D60_2_D65 => sRGB
const mat3 ACESOutputMat = mat3
(
    1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602
);

// ACES tone map (faster approximation)
// see: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 toneMapACES_Narkowicz(vec3 color)
{
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
}


// ACES filmic tone map approximation
// see https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
vec3 RRTAndODTFit(vec3 color)
{
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}


// tone mapping
vec3 toneMapACES_Hill(vec3 color)
{
    color = ACESInputMat * color;

    // Apply RRT and ODT
    color = RRTAndODTFit(color);

    color = ACESOutputMat * color;

    // Clamp to [0, 1]
    color = clamp(color, 0.0, 1.0);

    return color;
}

// Khronos Neutral tonemapping
// https://github.com/KhronosGroup/ToneMapping/tree/main
// Input color is non-negative and resides in the Linear Rec. 709 color space.
// Output color is also Linear Rec. 709, but in the [0, 1] range.
vec3 PBRNeutralToneMapping( vec3 color )
{
  const float startCompression = 0.8 - 0.04;
  const float desaturation = 0.15;

  float x = min(color.r, min(color.g, color.b));
  float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  color -= offset;

  float peak = max(color.r, max(color.g, color.b));
  if (peak < startCompression) return color;

  const float d = 1. - startCompression;
  float newPeak = 1. - d * d / (peak + d - startCompression);
  color *= newPeak / peak;

  float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
  return mix(color, newPeak * vec3(1, 1, 1), g);
}

uniform float exposure;
uniform float tonemap_mix;
uniform int tonemap_type;

#if defined(LUMINOUS_LOOK)
// <Luminous> Photo mode's Look: grading on the exposed scene-linear colour,
// then Firestorm's curves or two more. Set from LuminousPhotoMode::bindLookUniforms;
// at its defaults it changes nothing.
uniform mat3 luminous_look_balance;      // white balance (von Kries, in LMS), linear RGB to linear RGB
uniform float luminous_look_ev;          // exposure compensation, as a factor
uniform float luminous_look_contrast;    // around middle grey, in stops
uniform float luminous_look_saturation;
uniform float luminous_look_mono;        // 0 colour .. 1 black and white
uniform vec3 luminous_look_mono_weights; // the black and white's filter, summing to 1
uniform float luminous_look_tone;        // 0 neutral .. 1 sepia, on the finished picture
uniform float luminous_look_highlights;  // -1 .. 1, in stops of 1.5 at most
uniform float luminous_look_shadows;     // -1 .. 1
uniform float luminous_look_whites;      // -1 .. 1, the brightest end
uniform float luminous_look_blacks;      // -1 .. 1, the deepest end
uniform vec3 luminous_look_balance_shadows;    // log2 gains per channel
uniform vec3 luminous_look_balance_midtones;
uniform vec3 luminous_look_balance_highlights;
uniform float luminous_look_vibrance;    // -1 .. 1
uniform float luminous_look_grain;       // 0 .. 1
uniform float luminous_look_grain_size;  // pixels of a 1080-line frame

const vec3 LUMINOUS_LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 luminousWorn(vec3 c, vec2 frame_uv, vec2 res); // luminousWornF.glsl

// Light leak: light that reached the film past the camera's seal, coming
// in from the frame's edge -- a soft glow, or a band down the edge with a
// sharp inner side and stripes along it (Hardness) -- strongest where it
// comes in. Taken as film takes light: faint, it is its colour; stronger,
// the film's other layers fill too, so a red leak goes on through orange
// and yellow to white where it comes in. Rainbow turns its colour further
// in. Screened onto the finished picture, as a leak lies over a photograph,
// so a dark part takes its colour; a light part a little of it. Laid out on
// the picture, as Worn photo is, from a hash: the same live and in every
// frame of a render.
uniform float luminous_leak;          // 0 .. 1; 0 draws nothing
uniform vec3 luminous_leak_colour;    // its colour where faint, brightest channel 1
uniform float luminous_leak_place;    // where it comes in, once round the frame: 0 the right edge's middle, 0.25 the top's
uniform float luminous_leak_size;     // 0 .. 1, how far in it reaches
uniform float luminous_leak_hardness; // 0 a soft glow .. 1 a band with a sharp inner side
uniform float luminous_leak_rainbow;  // 0 .. 1, its colour turning further in
uniform float luminous_leak_seed;     // which shape: a whole number
uniform float luminous_frame_scale;   // the frame this many picture heights tall: a render's margin, or a shape chosen live
uniform float luminous_picture_aspect; // the picture's width over height; 0: the frame's (a shape chosen on the Render tab, live)

vec3 luminousToSrgb(vec3 c);
vec3 luminousFromSrgb(vec3 c);

float luminousLeakHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float luminousLeakNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(luminousLeakHash(i), luminousLeakHash(i + vec2(1.0, 0.0)), f.x),
               mix(luminousLeakHash(i + vec2(0.0, 1.0)), luminousLeakHash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// a turn of the hue round the grey axis, in radians
vec3 luminousLeakTurn(vec3 c, float a)
{
    const vec3 k = vec3(0.57735027);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}

vec3 luminousLightLeak(vec3 c)
{
    if (luminous_leak <= 0.0)
    {
        return c;
    }
    float frame_scale = luminous_frame_scale > 0.0 ? luminous_frame_scale : 1.0;
    float aspect = screen_res.x / max(screen_res.y, 1.0);
    vec2 half_size = vec2(0.5 * (luminous_picture_aspect > 0.0 ? luminous_picture_aspect : aspect), 0.5);
    vec2 q = (vary_fragcoord - 0.5) * frame_scale * vec2(aspect, 1.0); // picture heights from its middle
    // where the frame's edge meets the chosen direction, and which way is in
    float angle = luminous_leak_place * 6.2831853;
    vec2 dir = vec2(cos(angle), sin(angle));
    vec2 hit = half_size / max(abs(dir), vec2(1e-4));
    vec2 at = dir * min(hit.x, hit.y);
    vec2 inward = hit.x < hit.y ? vec2(-sign(dir.x), 0.0) : vec2(0.0, -sign(dir.y));
    vec2 along = vec2(-inward.y, inward.x);
    float size = mix(0.15, 0.9, luminous_leak_size); // picture heights
    float hard = clamp(luminous_leak_hardness, 0.0, 1.0);
    float rainbow = clamp(luminous_leak_rainbow, 0.0, 1.0);
    vec2 o = vec2(13.0, 29.0) * luminous_leak_seed;
    vec2 d = q - at;
    float depth = max(dot(d, inward), 0.0); // into the picture
    float side = dot(d, along);             // along the edge
    // how far in it reaches varies along the edge
    float reach = size * (0.7 + 0.6 * luminousLeakNoise(vec2(side / size * 1.5, 3.0) + o)) * (1.0 + rainbow);
    // soft: a glow that fades in and along the edge; hard: a band most of
    // the edge's length, fading toward its sharp inner side
    float glow = exp(-2.0 * pow(depth / reach, 2.0) - pow(side / (1.6 * size), 2.0));
    float band = (1.0 - smoothstep(reach * mix(0.3, 0.9, hard), reach, depth))
               * mix(1.0, exp(-1.5 * depth / reach), 0.6) * exp(-pow(side / (3.5 * size), 2.0));
    float shape = mix(glow, band, hard);
    // stripes along the edge, and the light uneven over its length
    shape *= mix(1.0, 0.5 + 0.9 * smoothstep(0.25, 0.75, luminousLeakNoise(vec2(depth / size * 6.0, side / size * 0.25) + o + 7.0)), 0.3 + 0.7 * hard);
    shape *= 0.8 + 0.4 * luminousLeakNoise(q / size * 2.0 + o + 11.0);
    // brightest where it comes in
    float core = exp(-pow(depth / (0.2 * reach), 2.0));
    float exposure = shape * luminous_leak * 1.6 * (1.0 + 2.5 * core);
    // the film's layers: the leak's colour first, the others as it grows
    vec3 sens = luminous_leak_colour / max(max(luminous_leak_colour.r, luminous_leak_colour.g), max(luminous_leak_colour.b, 1e-3)) * 1.4 + 0.05;
    vec3 light = 1.0 - exp(-exposure * sens);
    if (rainbow > 0.0)
    {
        light = clamp(luminousLeakTurn(light, rainbow * 3.5 * smoothstep(0.1, 1.0, depth / reach)), 0.0, 1.0);
    }
    vec3 s = luminousToSrgb(clamp(c, 0.0, 1.0));
    s = 1.0 - (1.0 - s) * (1.0 - light);
    // and where it is strong, its colour over the picture's -- screened alone,
    // a red leak over a blue sky turns pink -- most for a band, least for a
    // glow and a rainbow, which the picture shows through
    float l = dot(s, LUMINOUS_LUMA);
    vec3 tinted = clamp(light * (l / max(dot(light, LUMINOUS_LUMA), 0.05)), 0.0, 1.0);
    s = mix(s, tinted, clamp(exposure * 0.6, 0.0, 1.0) * mix(0.3, 0.75, hard) * (1.0 - 0.5 * rainbow));
    return luminousFromSrgb(s);
}

vec3 luminousGrade(vec3 c)
{
    c = max(luminous_look_balance * c, vec3(0.0)) * luminous_look_ev;

    // Highlights and shadows: a gain in stops, weighted by how far above or
    // below middle grey the light is, so each moves its own end of the range
    // and middle grey stays put. Hue is kept: all three channels move together.
    float luma = dot(c, LUMINOUS_LUMA);
    float stops = log2(max(luma, 1e-6) / 0.18);
    float gain = luminous_look_highlights * 1.5 * smoothstep(0.0, 3.0, stops)
               + luminous_look_shadows * 1.5 * (1.0 - smoothstep(-5.0, 0.0, stops))
               + luminous_look_whites * 1.5 * smoothstep(1.5, 4.5, stops)
               + luminous_look_blacks * 1.5 * (1.0 - smoothstep(-8.0, -3.0, stops));
    c *= exp2(gain);

    // Colour balance in three ranges, blended by the same distance from middle
    // grey, each a gain per channel; the luminance is kept, so a tint does not
    // brighten or darken.
    luma = dot(c, LUMINOUS_LUMA);
    stops = log2(max(luma, 1e-6) / 0.18);
    float w_shadows = 1.0 - smoothstep(-4.0, 0.0, stops);
    float w_highlights = smoothstep(0.0, 4.0, stops);
    float w_midtones = max(1.0 - w_shadows - w_highlights, 0.0);
    vec3 tinted = c * exp2(luminous_look_balance_shadows * w_shadows
                         + luminous_look_balance_midtones * w_midtones
                         + luminous_look_balance_highlights * w_highlights);
    c = tinted * (luma / max(dot(tinted, LUMINOUS_LUMA), 1e-6));

    // Saturation for all colour; vibrance most for what has least.
    luma = dot(c, LUMINOUS_LUMA);
    float peak = max(c.r, max(c.g, c.b));
    float chroma = peak > 1e-6 ? (peak - min(c.r, min(c.g, c.b))) / peak : 0.0;
    float sat = luminous_look_saturation * (1.0 + luminous_look_vibrance * (1.0 - chroma));
    c = max(mix(vec3(luma), c, sat), vec3(0.0));

    // Black and white through a coloured filter, as on a camera: a red one
    // darkens blue sky, a green one lightens leaves.
    float mono = max(dot(c, luminous_look_mono_weights), 0.0);
    c = mix(c, vec3(mono), luminous_look_mono);

    // Contrast in stops around 0.18, the way film's curve is steepened: a
    // colour keeps its hue and only its distance from middle grey changes.
    c = 0.18 * pow(c / 0.18 + 1e-6, vec3(luminous_look_contrast));
    return c;
}

float luminousHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// A colour lookup table (.cube), applied to the finished picture as such
// tables expect: sRGB-encoded in and out. Strength 0 leaves it out.
uniform sampler3D luminous_lut;
uniform float luminous_lut_strength;
uniform float luminous_lut_size;

vec3 luminousToSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 luminousFromSrgb(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// HSL by colour range, on the finished picture, as a photo editor's: eight
// ranges centred on red, orange, yellow, green, aqua, blue, purple and
// magenta, each with a hue shift (up to 30 degrees), a saturation and a
// lightness. A pixel belongs to the two nearest ranges, blended by distance,
// and the more saturated it is the more it is moved, so greys stay grey.
uniform vec3 luminous_hsl[8]; // hue shift -1..1, saturation -1..1, lightness -1..1

vec3 luminousRgbToHsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}

vec3 luminousHsvToRgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

vec3 luminousHsl(vec3 c)
{
    const float CENTRES[9] = float[9](0.0, 30.0, 60.0, 120.0, 180.0, 240.0, 270.0, 300.0, 360.0);
    vec3 hsv = luminousRgbToHsv(clamp(c, 0.0, 1.0));
    float h = hsv.x * 360.0;
    int i = 0;
    for (int k = 0; k < 8; ++k)
    {
        if (h >= CENTRES[k])
        {
            i = k;
        }
    }
    float t = (h - CENTRES[i]) / (CENTRES[i + 1] - CENTRES[i]);
    t = t * t * (3.0 - 2.0 * t);
    vec3 adj = mix(luminous_hsl[i], luminous_hsl[(i + 1) % 8], t) * smoothstep(0.0, 0.25, hsv.y);
    if (dot(abs(adj), vec3(1.0)) < 1e-4)
    {
        return c;
    }
    float before = dot(c, vec3(0.2126, 0.7152, 0.0722));
    hsv.x = fract(hsv.x + adj.x * 30.0 / 360.0 + 1.0);
    hsv.y = clamp(hsv.y * (1.0 + adj.y), 0.0, 1.0);
    vec3 rgb = luminousHsvToRgb(hsv);
    // the hue and saturation change keep the brightness; lightness sets it
    rgb *= before / max(dot(rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-5);
    return rgb * (1.0 + adj.z * 0.6);
}

vec3 luminousFinish(vec3 c)
{
    c = luminousHsl(c);
    if (luminous_lut_strength > 0.0)
    {
        vec3 uv = luminousToSrgb(c) * ((luminous_lut_size - 1.0) / luminous_lut_size) + 0.5 / luminous_lut_size;
        vec3 graded = luminousFromSrgb(textureLod(luminous_lut, uv, 0.0).rgb);
        c = mix(c, graded, luminous_lut_strength);
    }

    // Sepia: the picture's luminance, toned warm, kept as bright as it was.
    float luma = dot(c, LUMINOUS_LUMA);
    vec3 sepia = luma * vec3(1.18, 1.0, 0.72);
    c = mix(c, sepia, luminous_look_tone);

    c = luminousLightLeak(c);

    // Film grain: soft noise in clumps of the chosen size, scaled with the
    // frame so a large snapshot grains as the screen does; strongest in the
    // middle tones, as film's is, and the same for all three channels.
    if (luminous_look_grain > 0.0)
    {
        vec2 cell = gl_FragCoord.xy / max(luminous_look_grain_size * screen_res.y / 1080.0, 1.0);
        vec2 i = floor(cell);
        vec2 f = smoothstep(0.0, 1.0, fract(cell));
        float n = mix(mix(luminousHash(i), luminousHash(i + vec2(1.0, 0.0)), f.x),
                      mix(luminousHash(i + vec2(0.0, 1.0)), luminousHash(i + vec2(1.0, 1.0)), f.x), f.y);
        float l = sqrt(clamp(dot(c, LUMINOUS_LUMA), 0.0, 1.0));
        c *= 1.0 + (n - 0.5) * luminous_look_grain * 0.6 * (4.0 * l * (1.0 - l));
    }
    // Worn photo (luminousWornF.glsl), after the grain
    return luminousWorn(c, vary_fragcoord, screen_res);
}

// AgX (Troy Sobotka), as fitted by Benjamin Wrensch: into AgX's working space,
// a log2 encoding over 16.5 stops, a sixth-order fit of its sigmoid, and out
// again. It keeps hue in bright, saturated light, where per-channel curves
// skew to yellow and cyan, and lets the brightest light go to white.
const mat3 LUMINOUS_AGX_IN = mat3(
    0.842479062253094, 0.0423282422610123, 0.0423756549057051,
    0.0784335999999992, 0.878468636469772, 0.0784336,
    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
const mat3 LUMINOUS_AGX_OUT = mat3(
    1.19687900512017, -0.0528968517574562, -0.0529716355144438,
    -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
    -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

vec3 luminousAgX(vec3 c, bool punchy)
{
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    c = LUMINOUS_AGX_IN * max(c, vec3(1e-10));
    c = clamp(log2(c), min_ev, max_ev);
    c = (c - min_ev) / (max_ev - min_ev);
    vec3 x2 = c * c;
    vec3 x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    if (punchy)
    {
        // AgX's "Punchy" look: a steeper curve and more colour, as an ASC CDL
        // power and saturation.
        c = pow(max(c, vec3(0.0)), vec3(1.35));
        float luma = dot(c, LUMINOUS_LUMA);
        c = luma + 1.4 * (c - luma);
    }
    c = LUMINOUS_AGX_OUT * c;
    // AgX's output is display-encoded (2.2); Firestorm encodes after this.
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

// John Hable's filmic curve (Uncharted 2), with his exposure bias and white.
vec3 luminousHablePartial(vec3 x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Tony McMapface (Tomasz Stachowiak, 2022, MIT): a neutral tonemapper baked
// into a 48^3 lookup table, which keeps hue in bright light and lets it
// desaturate toward white as film does. Its own sampling, from its shader.
uniform sampler3D luminous_tonemap_lut;

vec3 luminousMcMapface(vec3 c)
{
    const float DIMS = 48.0;
    vec3 encoded = c / (c + 1.0);
    vec3 uv = encoded * ((DIMS - 1.0) / DIMS) + 0.5 / DIMS;
    return textureLod(luminous_tonemap_lut, uv, 0.0).rgb;
}

// Hajime Uchimura's curve for Gran Turismo Sport (CEDEC 2017, "HDR Theory
// and Practice"), with its published defaults: a toe, a long straight middle
// that keeps contrast and colour as they are, and a smooth shoulder to white.
vec3 luminousGranTurismo(vec3 x)
{
    const float P = 1.0;  // the brightest output
    const float a = 1.0;  // contrast of the straight part
    const float m = 0.22; // where it starts
    const float l = 0.4;  // its length
    const float c = 1.33; // the toe's curve
    const float b = 0.0;  // the toe's pedestal
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;
    vec3 w0 = 1.0 - smoothstep(vec3(0.0), vec3(m), x);
    vec3 w2 = step(vec3(m + l0), x);
    vec3 w1 = 1.0 - w0 - w2;
    vec3 T = m * pow(max(x, vec3(0.0)) / m, vec3(c)) + b;
    vec3 S = P - (P - S1) * exp(CP * (x - S0));
    vec3 L = m + a * (x - m);
    return clamp(T * w0 + L * w1 + S * w2, 0.0, 1.0);
}

vec3 luminousFilmic(vec3 c)
{
    const float W = 11.2;
    return clamp(luminousHablePartial(c * 2.0) / luminousHablePartial(vec3(W)), 0.0, 1.0);
}

uniform int luminous_look_on;    // 0 for a snapshot taken without post-processing

// Under a legacy sky (no probe ambiance) Firestorm draws with its NO_POST
// programs, which only clamp: no exposure, no tonemapping. The Look's grading
// applies there too, except in a snapshot asked for without post-processing;
// its tonemapper does not -- measured, AgX and Filmic on a legacy sky's light,
// which is not scene-referred, darken it and push skin red.
vec3 luminousNoPost(vec3 c)
{
    if (luminous_look_on == 0)
    {
        return clamp(c, 0.0, 1.0);
    }
    return clamp(luminousFinish(clamp(luminousGrade(c), 0.0, 1.0)), 0.0, 1.0);
}
#else
// <Luminous> outside photo mode, exactly Firestorm's NO_POST clamp
vec3 luminousNoPost(vec3 c)
{
    return clamp(c, vec3(0.0), vec3(1.0));
}
#endif


vec3 toneMap(vec3 color)
{
#ifndef NO_POST
    vec3 linear_input_color = color;

    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    float final_exposure = exposure * exp_scale;
    vec3 exposed_color = color * final_exposure;
#if defined(LUMINOUS_LOOK)
    exposed_color = luminousGrade(exposed_color); // <Luminous/>
#endif

    vec3 tonemapped_color = exposed_color;
    switch(tonemap_type)
    {
    case 0:
        tonemapped_color = PBRNeutralToneMapping(exposed_color);
        break;
    case 1:
        tonemapped_color = toneMapACES_Hill(exposed_color);
        break;
#if defined(LUMINOUS_LOOK) // <Luminous> photo mode's curves
    case 2:
        tonemapped_color = luminousAgX(exposed_color, false);
        break;
    case 3:
        tonemapped_color = luminousAgX(exposed_color, true);
        break;
    case 4:
        tonemapped_color = luminousFilmic(exposed_color);
        break;
    case 5:
        tonemapped_color = luminousGranTurismo(exposed_color);
        break;
    case 6:
        tonemapped_color = luminousMcMapface(max(exposed_color, vec3(0.0)));
        break;
#endif // </Luminous>
    }

#if defined(LUMINOUS_LOOK)
    vec3 exposed_linear_input = exposed_color; // <Luminous/> graded
#else
    vec3 exposed_linear_input = linear_input_color * final_exposure;
#endif
    color = mix(exposed_linear_input, tonemapped_color, tonemap_mix);

    color = clamp(color, 0.0, 1.0);
#if defined(LUMINOUS_LOOK)
    color = clamp(luminousFinish(color), 0.0, 1.0); // <Luminous/>
#endif
#else
    color *= exposure * texture(exposureMap, vec2(0.5,0.5)).r;
    color = clamp(color, 0.0, 1.0);
#endif

    return color;
}


vec3 toneMapNoExposure(vec3 color)
{
#ifndef NO_POST
    vec3 linear_input_color = color;

    vec3 tonemapped_color = color;
    switch(tonemap_type)
    {
    case 0:
        tonemapped_color = PBRNeutralToneMapping(color);
        break;
    case 1:
        tonemapped_color = toneMapACES_Hill(color);
        break;
    }

    color = mix(linear_input_color, tonemapped_color, tonemap_mix);

    color = clamp(color, 0.0, 1.0);
#else
     color = clamp(color, 0.0, 1.0);
#endif

    return color;
}


//===============================================================

void debugExposure(inout vec3 color)
{
    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    exp_scale *= 0.5;
    if (abs(vary_fragcoord.y-exp_scale) < 0.01 && vary_fragcoord.x < 0.1)
    {
        color = vec3(1,0,0);
    }
}
