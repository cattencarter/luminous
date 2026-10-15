/**
 * @file luminousVignetteF.glsl
 * @brief Photo mode: a lens-like vignette, applied to light before tonemapping.
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

// A darkening factor only: drawn with multiplicative blending into the HDR
// frame, so it never reads the frame it darkens. It runs after the exposure
// has been measured, so the meter does not brighten the picture to make up for
// darker corners, and before tonemapping, so it falls off as light does through
// a lens rather than as a dark overlay on finished colours.
//
// Firestorm's own vignette (FSRenderVignette, hidden) is a separate pass, left
// as it is: it follows the frame's shape and works on display values after
// tonemapping.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform float luminous_vig_amount;     // 0..1: how much the corners darken
uniform float luminous_vig_size;       // where the darkening is half, 0 centre .. 1 corner
uniform float luminous_vig_softness;   // 0 a hard edge .. 1 very gradual
uniform float luminous_vig_roundness;  // 1 round like a lens, 0 follows the frame
uniform float luminous_aspect;         // frame width / height
uniform float luminous_frame_scale;    // a render's margin: the frame this much larger than the picture

void main()
{
    // Distance from the centre, with the corner at 1 whatever the shape.
    vec2 d = (vary_fragcoord.xy - 0.5) * luminous_frame_scale;
    vec2 scale = vec2(mix(1.0, luminous_aspect, luminous_vig_roundness), 1.0);
    float r = length(d * scale) / length(0.5 * scale);

    float half_width = 0.5 * max(luminous_vig_softness, 0.02);
    float dark = smoothstep(luminous_vig_size - half_width, luminous_vig_size + half_width, r);
    float factor = 1.0 - luminous_vig_amount * dark;

    frag_color = vec4(vec3(clamp(factor, 0.0, 1.0)), 1.0);
}
