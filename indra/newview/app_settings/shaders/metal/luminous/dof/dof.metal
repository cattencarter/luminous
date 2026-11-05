// Photo mode's depth of field (Enhance) on Metal: a gather at full resolution in HDR, after Dennis
// Gustafsson, "Bokeh depth of field in a single pass" (2018), with Luminous's tiles.
//
// The source is what Firestorm's circle-of-confusion pass (cofF.glsl) wrote: the HDR colour, and in
// alpha the signed blur radius in pixels, packed as cof / max_cof * 0.5 + 0.5 -- positive in front of
// the focus, negative behind it. The radius is monotonic in depth, so it also says which of two pixels
// is nearer, and no depth needs reading.
//
// Every pixel looks at a Vogel disc of samples out to the largest blur there can be, and takes a sample
// when that sample's own blur reaches this far. A sample from behind the pixel reaches no further than
// twice the pixel's own blur, so a blurred background never spreads onto a subject in focus. The samples
// read a mip level as coarse as their spacing, so a highlight smaller than the gaps between them still
// lands on every pixel it should and opens into a full disc. The spiral runs outward and a pixel stops at
// the last sample the largest blur near it can reach: every sample further out would count for nothing.
//
// The passes, in one compute pass and one render pass:
// 1. luminous_dof_tiles: per 16 x 16 tile, the largest blur in it and its coarse key (the smallest blur
//    in it, signed by the side of the focus it lies on, or 0 when it has both sides or a sharp pixel).
// 2. luminous_dof_rows, luminous_dof_classify: those values combined over the tiles a blur can reach
//    from, separably; each tile's kind: sharp (nothing to gather), full, or -- live, where every blur
//    in and round the tile is large -- half or quarter size, its tile listed for the coarse gather.
// 3. luminous_dof_coarse: the listed tiles gathered at half or quarter size, with their border texels.
// 4. luminous_dof_composite_fs, into the HDR frame (colour only): sharp tiles keep the frame, full tiles
//    gather at each pixel, coarse tiles are scaled up bilinearly.
// The mip levels come from the common single-pass downsampler into a texture of their own, so the
// source needs none.
//
// In a frame drawn for MetalFX temporal the camera is jittered by a sub-pixel offset each frame, and a blur
// gathered from that frame changes its shape at focus edges from frame to frame, which the scaler passes on.
// There (p.layer) the blur goes into a layer of its own instead, each texel the place the camera without the
// jitter saw, read from the source that far on (p.jitterX, p.jitterY): the samples' part of each gather and its
// weight (luminous_dof_layer_fs, the coarse tiles alike). The frame settles the layer (averaged with the frames
// before, moved by the camera), and luminous_dof_settled_fs adds it back where this frame shows each place: the
// frame keeps the rest of its own pixel, so what is in focus stays jittered as the scene is.

#include <metal_stdlib>
#include "luminous/common/fullscreen.h"
#include "dofshared.h"

using namespace metal;

namespace luminous_dof
{
    constant float GOLDEN_ANGLE = 2.39996323;
    constant float PI = 3.14159265;
    constant float TWO_PI = 6.28318531;

    /// The blur as the gather reads it: without the foreground blur, no foreground.
    inline float cofOf(float a, constant LuminousDoFParams& p)
    {
        const float cof = (a * 2.0 - 1.0) * p.maxCoF;
        return p.front > 0.5 ? cof : min(cof, 0.0);
    }

    /// The aperture's edge at this angle (0 to 2 pi), as a fraction of its corners' radius.
    inline float aperture(float angle, float blades)
    {
        if (blades < 3.0)
        {
            return 1.0;
        }
        const float segment = TWO_PI / blades;
        const float a = fmod(angle, segment) - segment * 0.5;
        return cos(segment * 0.5) / cos(a);
    }

    /// The source at a mip level: level 0 is the source itself, level k > 0 the chain's level k - 1,
    /// filtered between levels as a trilinear sampler is. Levels past what the chain holds read its last.
    inline float4 sampleLod(texture2d<float> source, texture2d<float> chain, float2 uv, float lod, int levels)
    {
        constexpr sampler linear(filter::linear, address::clamp_to_edge);
        constexpr sampler trilinear(filter::linear, mip_filter::linear, address::clamp_to_edge);
        lod = min(lod, float(levels));
        if (lod < 1.0)
        {
            const float4 a = source.sample(linear, uv, level(0.0));
            if (lod <= 0.0 || levels < 1)
            {
                return a;
            }
            return mix(a, chain.sample(linear, uv, level(0.0)), lod);
        }
        return chain.sample(trilinear, uv, level(lod - 1.0));
    }

    /// How much of a spiral of `total` samples at `lod` a pixel walks, when the largest blur that can
    /// reach it is `far`; past the budget a thinner spiral, read at a coarser level to match.
    struct Spiral
    {
        float n;
        float lod;
        int samples;
    };

    inline Spiral spiral(float far, int total, float lod, int budget, float maxCoF)
    {
        Spiral s;
        s.n = float(total);
        s.lod = lod;
        // Sample i lies at sqrt((i + 0.5) / n) * max_cof; one at or past the largest blur near this
        // pixel plus the half pixel of the soft edge has a reach of exactly 0.
        const float reach = far + 0.5;
        const float share = min(reach * reach / (maxCoF * maxCoF), 1.0);
        s.samples = clamp(int(ceil(s.n * share - 0.5)), 0, total);
        if (s.samples > budget)
        {
            s.n = float(budget) / share;
            s.samples = budget;
            s.lod = max(0.0, log2(maxCoF * sqrt(PI / s.n)));
        }
        return s;
    }

    /// The gather at x, in the source's pixels (a pixel's centre is at i + 0.5), around `centre`. centre_share:
    /// the centre's weight in the mean, the samples' the rest.
    inline float3 gather(texture2d<float> source, texture2d<float> chain, float2 x, float4 centre, Spiral s,
                         constant LuminousDoFParams& p, thread float& centre_share)
    {
        const float2 texel = 1.0 / p.screenRes;
        const float2 tc = x * texel;
        const float centre_cof = cofOf(centre.a, p);
        const float behind_most = abs(centre_cof) * 2.0;

        // The running mean of what has been gathered. A sample that does not reach this pixel counts as
        // that mean, so it changes nothing; the first sample after the centre weighs 1/2, the next 1/3.
        float3 mean = centre.rgb;
        centre_share = 1.0;
        float angle = 0.0;
        for (int i = 0; i < s.samples; ++i)
        {
            const float radius = sqrt((float(i) + 0.5) / s.n) * p.maxCoF;
            const float2 offset = float2(cos(angle), sin(angle)) * radius;

            const float4 sample = sampleLod(source, chain, tc + offset * texel, s.lod, p.levels);
            const float cof = cofOf(sample.a, p);
            float size = abs(cof);
            if (cof < centre_cof)
            {
                size = min(size, behind_most); // behind this pixel
            }
            size *= aperture(angle, p.blades);

            const float reach = smoothstep(radius - 0.5, radius + 0.5, size);
            mean += (sample.rgb - mean) * (reach / float(i + 2));
            centre_share *= 1.0 - reach / float(i + 2);

            angle += GOLDEN_ANGLE;
            angle = angle >= TWO_PI ? angle - TWO_PI : angle;
        }
        return mean;
    }

    inline uint tileIndex(uint2 tile, constant LuminousDoFParams& p)
    {
        return tile.y * p.tiles.x + tile.x;
    }

    /// Where a texel at x reads the source: in the layer, where this frame shows the place the camera without the
    /// jitter saw at x; else x.
    inline float2 readAt(float2 x, constant LuminousDoFParams& p)
    {
        return x + float2(p.jitterX, p.jitterY);
    }

    /// The source's pixel at x, the nearest.
    inline float4 centreAt(texture2d<float> source, float2 x)
    {
        const uint2 size = uint2(source.get_width(), source.get_height());
        return source.read(min(uint2(max(x, float2(0.0))), size - 1));
    }

    /// A gather as the layer holds it: the samples' part of the mean and their weight, so that a pixel where this
    /// frame shows the place stands in for the centre (frame * (1 - a) + rgb). Rounding never makes it negative.
    inline float4 layerOf(float3 mean, float centre_share, float3 centre)
    {
        return float4(max(mean - centre * centre_share, float3(0.0)), 1.0 - centre_share);
    }
}

/// One threadgroup per tile, a thread per pixel: the largest blur in the tile (as the translated tile
/// pass finds it), and its coarse key. The first threadgroup also sets the coarse lists' counts to 0.
kernel void luminous_dof_tiles(constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                               device float2* tiles [[buffer(LUMINOUS_DOF_BUFFER_TILES)]],
                               device atomic_uint* counts [[buffer(LUMINOUS_DOF_BUFFER_COUNTS)]],
                               texture2d<float, access::read> source [[texture(LUMINOUS_DOF_TEXTURE_SOURCE)]],
                               uint2 group [[threadgroup_position_in_grid]],
                               uint2 local [[thread_position_in_threadgroup]],
                               uint lane [[thread_index_in_simdgroup]],
                               uint simd [[simdgroup_index_in_threadgroup]],
                               uint simds [[simdgroups_per_threadgroup]])
{
    using namespace luminous_dof;
    threadgroup float4 partial[32];

    const uint2 size = uint2(source.get_width(), source.get_height());
    const uint2 pixel = min(group * uint(LUMINOUS_DOF_TILE) + local, size - 1);
    const float cof = cofOf(source.read(pixel).a, p);
    const float4 mine = float4(abs(cof), abs(cof), cof, cof); // largest, smallest, lowest, highest
    float4 r = float4(simd_max(mine.x), simd_min(mine.y), simd_min(mine.z), simd_max(mine.w));
    if (lane == 0)
    {
        partial[simd] = r;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (simd == 0)
    {
        r = lane < simds ? partial[lane] : partial[0];
        r = float4(simd_max(r.x), simd_min(r.y), simd_min(r.z), simd_max(r.w));
        if (lane == 0)
        {
            // Signed by the focus's side, and only when the tile lies wholly on one side.
            const float key = r.z > 0.0 ? r.y : (r.w < 0.0 ? -r.y : 0.0);
            tiles[tileIndex(group, p)] = float2(r.x, key);
            if (all(group == 0))
            {
                atomic_store_explicit(&counts[0], 0u, memory_order_relaxed);
                atomic_store_explicit(&counts[1], 0u, memory_order_relaxed);
            }
        }
    }
}

/// One thread per tile: along its row, the largest blur within the reach and within the coarse reach,
/// and the smallest coarse key of the tile and its two neighbours on each side of the focus.
kernel void luminous_dof_rows(constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                              device const float2* tiles [[buffer(LUMINOUS_DOF_BUFFER_TILES)]],
                              device float4* rows [[buffer(LUMINOUS_DOF_BUFFER_ROWS)]],
                              uint2 tile [[thread_position_in_grid]])
{
    using namespace luminous_dof;
    if (any(tile >= p.tiles))
    {
        return;
    }
    const int x = int(tile.x);
    const int last = int(p.tiles.x) - 1;
    const int far = max(p.reach, p.reachCoarse);
    const uint row = tile.y * p.tiles.x;
    float largest = 0.0;
    float largest_coarse = 0.0;
    float front = INFINITY;
    float back = INFINITY;
    for (int i = max(x - far, 0); i <= min(x + far, last); ++i)
    {
        const float2 t = tiles[row + uint(i)];
        const int d = abs(i - x);
        largest = d <= p.reach ? max(largest, t.x) : largest;
        largest_coarse = d <= p.reachCoarse ? max(largest_coarse, t.x) : largest_coarse;
        if (d <= 1)
        {
            front = min(front, max(t.y, 0.0));
            back = min(back, max(-t.y, 0.0));
        }
    }
    rows[tileIndex(tile, p)] = float4(largest, largest_coarse, front, back);
}

/// One thread per tile: the rows combined down the column, then how the tile is drawn, coarse tiles
/// listed for luminous_dof_coarse.
kernel void luminous_dof_classify(constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                                  device const float4* rows [[buffer(LUMINOUS_DOF_BUFFER_ROWS)]],
                                  device float2* state [[buffer(LUMINOUS_DOF_BUFFER_STATE)]],
                                  device atomic_uint* counts [[buffer(LUMINOUS_DOF_BUFFER_COUNTS)]],
                                  device uint* lists [[buffer(LUMINOUS_DOF_BUFFER_LISTS)]],
                                  uint2 tile [[thread_position_in_grid]])
{
    using namespace luminous_dof;
    if (any(tile >= p.tiles))
    {
        return;
    }
    const int y = int(tile.y);
    const int last = int(p.tiles.y) - 1;
    const int far_tiles = max(p.reach, p.reachCoarse);
    float largest = 0.0;
    float largest_coarse = 0.0;
    float front = INFINITY;
    float back = INFINITY;
    for (int j = max(y - far_tiles, 0); j <= min(y + far_tiles, last); ++j)
    {
        const float4 r = rows[uint(j) * p.tiles.x + tile.x];
        const int d = abs(j - y);
        largest = d <= p.reach ? max(largest, r.x) : largest;
        largest_coarse = d <= p.reachCoarse ? max(largest_coarse, r.y) : largest_coarse;
        if (d <= 1)
        {
            front = min(front, r.z);
            back = min(back, r.w);
        }
    }
    // The smallest blur in and round the tile, all on one side of the focus; 0 when not.
    const float coarse = max(front, back);

    const uint index = tileIndex(tile, p);
    const Spiral s = spiral(largest, p.samples, p.lod, p.budget, p.maxCoF);
    uint kind = LUMINOUS_DOF_KIND_FULL;
    float reach = largest;
    if (s.samples == 0)
    {
        kind = LUMINOUS_DOF_KIND_SHARP;
    }
    else if (p.picture == 0 && p.coarseQuarter > 0.0 && coarse >= p.coarseQuarter)
    {
        kind = LUMINOUS_DOF_KIND_QUARTER;
        reach = largest_coarse;
        const uint at = atomic_fetch_add_explicit(&counts[1], 1u, memory_order_relaxed);
        lists[p.tiles.x * p.tiles.y + at] = index;
    }
    else if (p.picture == 0 && p.coarseHalf > 0.0 && coarse >= p.coarseHalf)
    {
        kind = LUMINOUS_DOF_KIND_HALF;
        reach = largest_coarse;
        const uint at = atomic_fetch_add_explicit(&counts[0], 1u, memory_order_relaxed);
        lists[at] = index;
    }
    state[index] = float2(reach, float(kind));
}

/// The listed coarse tiles: the first threadgroups take the half-size tiles, two each (10 x 10 texels:
/// the tile's 8 x 8 and the border its scaling up reads), the next ones the quarter-size tiles, seven
/// each (6 x 6). Neighbouring tiles write their shared border texels alike. The rest return at once.
kernel void luminous_dof_coarse(constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                                device const float2* state [[buffer(LUMINOUS_DOF_BUFFER_STATE)]],
                                device const uint* counts [[buffer(LUMINOUS_DOF_BUFFER_COUNTS)]],
                                device const uint* lists [[buffer(LUMINOUS_DOF_BUFFER_LISTS)]],
                                texture2d<float> source [[texture(LUMINOUS_DOF_TEXTURE_SOURCE)]],
                                texture2d<float> chain [[texture(LUMINOUS_DOF_TEXTURE_CHAIN)]],
                                texture2d<float, access::write> half_size [[texture(LUMINOUS_DOF_TEXTURE_HALF)]],
                                texture2d<float, access::write> quarter_size [[texture(LUMINOUS_DOF_TEXTURE_QUARTER)]],
                                uint group [[threadgroup_position_in_grid]],
                                uint thread_index [[thread_index_in_threadgroup]])
{
    using namespace luminous_dof;
    const uint half_count = counts[0];
    const uint quarter_count = counts[1];
    const uint half_groups = (half_count + LUMINOUS_DOF_HALF_TILES_PER_GROUP - 1) / LUMINOUS_DOF_HALF_TILES_PER_GROUP;

    uint scale;
    uint side;  // texels on a side, the border included
    uint entry; // in its list
    uint list;  // where its list starts
    if (group < half_groups)
    {
        scale = 2;
        side = LUMINOUS_DOF_TILE / 2 + 2;
        const uint slot = thread_index / (side * side);
        if (slot >= LUMINOUS_DOF_HALF_TILES_PER_GROUP)
        {
            return;
        }
        entry = group * LUMINOUS_DOF_HALF_TILES_PER_GROUP + slot;
        if (entry >= half_count)
        {
            return;
        }
        list = 0;
    }
    else
    {
        scale = 4;
        side = LUMINOUS_DOF_TILE / 4 + 2;
        const uint slot = thread_index / (side * side);
        if (slot >= LUMINOUS_DOF_QUARTER_TILES_PER_GROUP)
        {
            return;
        }
        entry = (group - half_groups) * LUMINOUS_DOF_QUARTER_TILES_PER_GROUP + slot;
        if (entry >= quarter_count)
        {
            return;
        }
        list = p.tiles.x * p.tiles.y;
    }

    const uint index = lists[list + entry];
    const uint2 tile = uint2(index % p.tiles.x, index / p.tiles.x);
    const uint i = thread_index % (side * side);
    // The tile's texels start at tile * 16 / scale; the border reaches one texel before.
    const int2 texel = int2(tile * (LUMINOUS_DOF_TILE / scale)) - 1 + int2(i % side, i / side);
    const uint2 size = scale == 2 ? uint2(half_size.get_width(), half_size.get_height())
                                  : uint2(quarter_size.get_width(), quarter_size.get_height());
    if (any(texel < 0) || any(uint2(texel) >= size))
    {
        return;
    }

    // The texel's centre in the source's pixels, and the pixel there as the centre; in the layer, that far on.
    const float2 x = readAt((float2(texel) + 0.5) * float(scale), p);
    const float4 centre = centreAt(source, x);
    const int total = scale == 2 ? p.halfSamples : p.quarterSamples;
    const float lod = scale == 2 ? p.halfLod : p.quarterLod;
    const Spiral s = spiral(state[index].x, total, lod, min(p.budget, total), p.maxCoF);
    float centre_share;
    const float3 mean = gather(source, chain, x, centre, s, p, centre_share);
    const float4 colour = p.layer != 0 ? layerOf(mean, centre_share, centre.rgb) : float4(mean, 1.0);
    if (scale == 2)
    {
        half_size.write(colour, uint2(texel));
    }
    else
    {
        quarter_size.write(colour, uint2(texel));
    }
}

/// The blur into the HDR frame, colour only: sharp tiles keep the frame, full tiles gather at each
/// pixel as the translated gather does, coarse tiles are scaled up from their texture.
fragment float4 luminous_dof_composite_fs(LuminousFullscreenOut in [[stage_in]],
                                          constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                                          device const float2* state [[buffer(LUMINOUS_DOF_BUFFER_STATE)]],
                                          texture2d<float> source [[texture(LUMINOUS_DOF_TEXTURE_SOURCE)]],
                                          texture2d<float> chain [[texture(LUMINOUS_DOF_TEXTURE_CHAIN)]],
                                          texture2d<float> half_size [[texture(LUMINOUS_DOF_TEXTURE_HALF)]],
                                          texture2d<float> quarter_size [[texture(LUMINOUS_DOF_TEXTURE_QUARTER)]])
{
    using namespace luminous_dof;
    // Texel centres at i + 0.5 in each texture's own texels: a half-size texel's centre lies at
    // 2 i + 1 in the source's pixels, a quarter-size one's at 4 i + 2.
    constexpr sampler scaled(coord::pixel, filter::linear, address::clamp_to_edge);

    const uint2 pixel = uint2(in.position.xy);
    const float2 here = state[tileIndex(pixel / uint(LUMINOUS_DOF_TILE), p)];
    const uint kind = uint(here.y);
    if (kind == LUMINOUS_DOF_KIND_SHARP)
    {
        discard_fragment();
        return float4(0.0);
    }
    if (kind == LUMINOUS_DOF_KIND_HALF)
    {
        return float4(half_size.sample(scaled, in.position.xy * 0.5).rgb, 1.0);
    }
    if (kind == LUMINOUS_DOF_KIND_QUARTER)
    {
        return float4(quarter_size.sample(scaled, in.position.xy * 0.25).rgb, 1.0);
    }
    const float4 centre = source.read(pixel);
    const Spiral s = spiral(here.x, p.samples, p.lod, p.budget, p.maxCoF);
    float centre_share;
    return float4(gather(source, chain, in.position.xy, centre, s, p, centre_share), 1.0);
}

/// The layer in a frame drawn for MetalFX temporal: each texel the place the camera without the jitter saw, the
/// samples' part of its gather and their weight (layerOf); the coarse tiles' textures hold the same. Sharp tiles
/// are left as the pass cleared them: nothing to add.
fragment float4 luminous_dof_layer_fs(LuminousFullscreenOut in [[stage_in]],
                                      constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                                      device const float2* state [[buffer(LUMINOUS_DOF_BUFFER_STATE)]],
                                      texture2d<float> source [[texture(LUMINOUS_DOF_TEXTURE_SOURCE)]],
                                      texture2d<float> chain [[texture(LUMINOUS_DOF_TEXTURE_CHAIN)]],
                                      texture2d<float> half_size [[texture(LUMINOUS_DOF_TEXTURE_HALF)]],
                                      texture2d<float> quarter_size [[texture(LUMINOUS_DOF_TEXTURE_QUARTER)]])
{
    using namespace luminous_dof;
    constexpr sampler scaled(coord::pixel, filter::linear, address::clamp_to_edge);

    const uint2 pixel = uint2(in.position.xy);
    const float2 here = state[tileIndex(pixel / uint(LUMINOUS_DOF_TILE), p)];
    const uint kind = uint(here.y);
    if (kind == LUMINOUS_DOF_KIND_SHARP)
    {
        discard_fragment();
        return float4(0.0);
    }
    if (kind == LUMINOUS_DOF_KIND_HALF)
    {
        return half_size.sample(scaled, in.position.xy * 0.5);
    }
    if (kind == LUMINOUS_DOF_KIND_QUARTER)
    {
        return quarter_size.sample(scaled, in.position.xy * 0.25);
    }
    const float2 x = readAt(in.position.xy, p);
    const float4 centre = centreAt(source, x);
    const Spiral s = spiral(here.x, p.samples, p.lod, p.budget, p.maxCoF);
    float centre_share;
    const float3 mean = gather(source, chain, x, centre, s, p, centre_share);
    return layerOf(mean, centre_share, centre.rgb);
}

/// The settled layer added back where this frame shows each place, at uv minus the jitter: blended One,
/// OneMinusSourceAlpha into the frame, colour only, so the frame keeps 1 - a of its own pixel.
fragment float4 luminous_dof_settled_fs(LuminousFullscreenOut in [[stage_in]],
                                        constant LuminousDoFParams& p [[buffer(LUMINOUS_DOF_BUFFER_PARAMS)]],
                                        texture2d<float> layer [[texture(LUMINOUS_DOF_TEXTURE_LAYER)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    return layer.sample(linear, in.uv - float2(p.jitterX, p.jitterY) / p.screenRes);
}
