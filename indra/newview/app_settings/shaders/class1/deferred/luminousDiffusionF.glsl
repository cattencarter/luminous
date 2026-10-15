/**
 * @file luminousDiffusionF.glsl
 * @brief Photo mode: soft diffusion and film halation, before tonemapping.
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

/*[EXTRA_CODE_HERE]*/

// Soft diffusion, as a Pro-Mist filter: light from brighter surroundings
// spills into each pixel -- highlights bloom softly into the shadows round
// them, and the picture softens and lifts a little -- without touching what
// is already as bright as its surroundings.
// Halation, as on film: light that passes through the emulsion reflects off
// the film base and comes back red-orange, so the brightest edges carry a warm
// glow just outside them.

out vec4 frag_color;

uniform sampler2D diffuseRect; // the frame, drawn anew into the other target
uniform sampler2D bloomMap;    // its blur, at half size
uniform sampler2D exposureMap; // the metered exposure
uniform float luminous_diffusion;
uniform float luminous_halation;

in vec2 vary_fragcoord;

void main()
{
    const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
    vec4 frame = texture(diffuseRect, vary_fragcoord);
    vec3 c = frame.rgb;
    vec3 around = texture(bloomMap, vary_fragcoord).rgb;
    vec3 spill = max(around - c, vec3(0.0));
    // Not clamped: bloom draws its glow above 0.3 through this pass, up to
    // three (renderBloom), so a clamp here would change bloom's glow too.
    c += spill * luminous_diffusion * 0.8;

    // Toward its top, halation takes in less bright edges and grows faster:
    // four times as strong there.
    // How bright is also measured after exposure, as the streaks' threshold
    // is: a lit panel in a dark room, lifted by the exposure, glows as the
    // sun does by day. The brighter of the two, so a day that the exposure
    // lowers keeps its halation.
    // Past 1, up to 2: up to two and a half times as strong again, from
    // somewhat less bright edges, and (in the blur, renderDiffusion) reaching
    // further; at 1 and below `more` is 0 and changes nothing.
    float h = min(luminous_halation, 1.0);
    float more = clamp(luminous_halation - 1.0, 0.0, 1.0);
    float lum = dot(around, LUMA);
    float exposed = max(lum, lum * texture(exposureMap, vec2(0.5)).r);
    float bright = smoothstep(mix(mix(0.8, 0.4, h * h), 0.25, more), mix(mix(4.0, 2.0, h * h), 1.5, more), exposed);
    c += spill * bright * vec3(1.0, 0.32, 0.12) * (h * 1.5 + h * h * h * 4.5) * (1.0 + 1.5 * more);
    frag_color = vec4(c, frame.a); // the alpha is Firestorm's glow mask: carried over
}
