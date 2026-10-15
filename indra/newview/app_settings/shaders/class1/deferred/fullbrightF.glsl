/**
 * @file deferred/fullbrightF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

out vec4 frag_color;

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cl);

#ifdef HAS_ALPHA_MASK
uniform float minimum_alpha;
#endif

#ifdef IS_ALPHA
uniform vec4 waterPlane;
void waterClip(vec3 pos);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
#endif

void mirrorClip(vec3 pos);

#if defined(HAS_SKIN)
// <Luminous> Alpha sorting in a render's frame of several: worn see-through
// pixels drawn solid or not at all, with their depth, by a threshold that steps
// evenly through the frames from an offset of the pixel's own -- so the
// frames' average is the alpha, and every layer sorts by depth, pixel by
// pixel (LLDrawPoolAlpha, LuminousPhotoMode::alphaSortStep).
uniform float luminous_alpha_step; // (frame + 0.5) / frames; 0 or below off (unset is 0)
// This draw's own number, 0-1, and the triangle's: layers over each other --
// two worn objects, drawn apart or, sharing a texture, in one draw -- get
// thresholds of their own. With one threshold for all, a pixel's layers
// would come or go together and count as one, and two layers of body hair
// would come out fainter than in Second Life.
uniform float luminous_alpha_seed;
// The render's two passes: 0 what covers at least half, by its coverage;
// 1 the faint parts, blended, behind the first and with no depth of their
// own (LLDrawPoolAlpha::renderPostDeferred).
uniform float luminous_alpha_faint;
float luminousCoverage(float alpha)
{
    if (luminous_alpha_step <= 0.0)
    {
        return alpha;
    }
    if ((alpha < 0.5) != (luminous_alpha_faint > 0.5))
    {
        discard; // the other pass's
    }
    if (luminous_alpha_faint > 0.5)
    {
        return alpha;
    }
    float layer = luminous_alpha_seed + float(gl_PrimitiveID & 4095) * 0.000244140625;
    vec3 p3 = fract(vec3(gl_FragCoord.xyx) * 0.1031 + layer * vec3(17.31, 31.17, 23.71));
    p3 += dot(p3, p3.yzx + 33.33);
    float offset = fract((p3.x + p3.y) * p3.z);
    if (alpha <= fract(offset + luminous_alpha_step))
    {
        discard;
    }
    return 1.0;
}
#endif // </Luminous>

void main()
{
    mirrorClip(vary_position);
#ifdef IS_ALPHA
    waterClip(vary_position.xyz);
#endif

#ifdef HAS_DIFFUSE_LOOKUP
    vec4 color = diffuseLookup(vary_texcoord0.xy);
#else
    vec4 color = texture(diffuseMap, vary_texcoord0.xy);
#endif

    float final_alpha = color.a * vertex_color.a;

#ifdef HAS_ALPHA_MASK
    if (color.a < minimum_alpha)
    {
        discard;
    }
#endif

    color.rgb *= vertex_color.rgb;

    vec3 pos = vary_position;

    color.a = final_alpha;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#ifdef IS_ALPHA

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

#endif

#endif

#if defined(HAS_SKIN)
    color.a = luminousCoverage(color.a); // <Luminous/>
#endif
    frag_color = max(color, vec4(0));
}

