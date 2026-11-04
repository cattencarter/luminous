/**
 * @file llrhiformatcases.h
 * @brief The upload and readback rows that GL and Metal share (P3-01c)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
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

#ifndef LL_LLRHIFORMATCASES_H
#define LL_LLRHIFORMATCASES_H

#include "../llrhi.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// One table for both backends (C3, C6): llrhigl_test.cpp sends each Both row through the GL device
// first, so its expected bytes are GL's, the oracle; llrhimetal_metal.mm's t35 sends every row
// through a Metal device. MetalOnly rows are GL's generic compressed formats with uncompressed
// input: Apple's GL keeps them block-compressed and lossy (GL_TEXTURE_COMPRESSED 1, by a run-time
// probe), so GL checks only LLImageGL's accounting of them. Rows whose GL result is
// implementation-defined are not in the table.

namespace LL::RHI::FormatCases
{
    // GL's values of the enums the rows use, so the Metal test needs no GL header;
    // llrhigl_test.cpp checks them against GL's.
    constexpr U32 GL_FORMAT_RGB = 0x1907;
    constexpr U32 GL_FORMAT_RGB8 = 0x8051;
    constexpr U32 GL_FORMAT_SRGB8 = 0x8C41;
    constexpr U32 GL_FORMAT_RGB16F = 0x881B;
    constexpr U32 GL_FORMAT_RGB32F = 0x8815;
    constexpr U32 GL_FORMAT_COMPRESSED_RED = 0x8225;
    constexpr U32 GL_FORMAT_COMPRESSED_RG = 0x8226;
    constexpr U32 GL_FORMAT_COMPRESSED_RGB = 0x84ED;
    constexpr U32 GL_FORMAT_COMPRESSED_RGBA = 0x84EE;
    constexpr U32 GL_FORMAT_COMPRESSED_SRGB_ALPHA = 0x8C49;

    enum class Scope
    {
        Both,
        MetalOnly,
    };

    struct FormatCase
    {
        std::string name;
        TextureDesc desc;            ///< format, glFormat, swizzle and size; one level, one layer
        PixelLayout upload;
        std::vector<U8> input;
        PixelLayout readback;
        std::vector<U8> expected;
        Scope scope = Scope::Both;
        S64 accountedBytes = 0;             ///< MetalOnly: LLImageGL::dataFormatBytes of glFormat and the size
    };

    inline PixelLayout layoutOf(PixelLayout::Components components, PixelLayout::Type type, bool swapBytes = false,
                                U32 rowLength = 0)
    {
        PixelLayout layout;
        layout.components = components;
        layout.type = type;
        layout.swapBytes = swapBytes;
        layout.rowLength = rowLength;
        return layout;
    }

    inline TextureDesc descOf(Format format, U32 glFormat, U32 width, U32 height, SwizzleRGBA swizzle = SwizzleRGBA())
    {
        TextureDesc desc;
        desc.format = format;
        desc.glFormat = glFormat;
        desc.width = width;
        desc.height = height;
        desc.usage = TEXTURE_SAMPLED | TEXTURE_READBACK;
        desc.swizzle = swizzle;
        return desc;
    }

    inline std::vector<U8> floatBytes(std::initializer_list<F32> values)
    {
        std::vector<U8> bytes(values.size() * 4);
        memcpy(bytes.data(), values.begin(), bytes.size());
        return bytes;
    }

    inline std::vector<U8> wordBytes(std::initializer_list<U16> values)
    {
        std::vector<U8> bytes(values.size() * 2);
        memcpy(bytes.data(), values.begin(), bytes.size());
        return bytes;
    }

    /// Every component's bytes reversed, as GL_UNPACK_SWAP_BYTES reads them.
    inline std::vector<U8> swapped(std::vector<U8> bytes, size_t size)
    {
        for (size_t at = 0; at + size <= bytes.size(); at += size)
        {
            for (size_t i = 0; i < size / 2; ++i)
            {
                std::swap(bytes[at + i], bytes[at + size - 1 - i]);
            }
        }
        return bytes;
    }

    inline std::vector<U8> pattern(size_t count, U32 seed)
    {
        std::vector<U8> bytes(count);
        for (size_t i = 0; i < count; ++i)
        {
            bytes[i] = U8(i * 37 + seed * 11 + 5);
        }
        return bytes;
    }

    /// RGB pixels as RGBA with the given alpha bytes (size bytes per component).
    inline std::vector<U8> withAlpha(const std::vector<U8>& rgb, size_t size, const std::vector<U8>& alpha)
    {
        std::vector<U8> rgba;
        for (size_t at = 0; at < rgb.size(); at += 3 * size)
        {
            rgba.insert(rgba.end(), rgb.begin() + at, rgb.begin() + at + 3 * size);
            rgba.insert(rgba.end(), alpha.begin(), alpha.end());
        }
        return rgba;
    }

    /// RGBA pixels with R and B swapped (BGRA's bytes).
    inline std::vector<U8> redBlueSwapped(std::vector<U8> bytes)
    {
        for (size_t at = 0; at + 4 <= bytes.size(); at += 4)
        {
            std::swap(bytes[at], bytes[at + 2]);
        }
        return bytes;
    }

    /// A float's half as Apple's GL makes it from GL_FLOAT for GL_RGB16F: truncated toward zero,
    /// subnormals kept, Inf above the largest half's exponent (a run-time probe of all 48
    /// values of each noise map and the constructed rows below).
    inline U16 halfAsGL(F32 value)
    {
        U32 bits = 0;
        memcpy(&bits, &value, 4);
        const U32 sign = (bits >> 16) & 0x8000;
        const S32 exponent = S32((bits >> 23) & 0xFF);
        const U32 mantissa = bits & 0x7FFFFF;
        if (exponent - 112 >= 31)
        {
            return U16(sign | 0x7C00);
        }
        if (exponent - 112 >= 1)
        {
            return U16(sign | (U32(exponent - 112) << 10) | (mantissa >> 13));
        }
        const S32 shift = 126 - exponent;
        return U16(exponent == 0 || shift > 24 ? sign : sign | ((mantissa | 0x800000) >> shift));
    }

    /// F32 noise in [0, 1), as P2-11a's noise maps are, and GL's halves of it, with alpha 1 for each
    /// RGB pixel.
    inline void noise(U32 seed, size_t pixels, std::vector<U8>& input, std::vector<U8>& halves)
    {
        std::vector<F32> values(pixels * 3);
        U32 state = seed;
        for (F32& value : values)
        {
            state = state * 1664525u + 1013904223u;
            value = F32(state >> 8) / F32(1 << 24);
        }
        input.resize(values.size() * 4);
        memcpy(input.data(), values.data(), input.size());
        std::vector<U16> out;
        for (size_t i = 0; i < values.size(); ++i)
        {
            out.push_back(halfAsGL(values[i]));
            if (i % 3 == 2)
            {
                out.push_back(0x3C00);
            }
        }
        halves.resize(out.size() * 2);
        memcpy(halves.data(), out.data(), halves.size());
    }

    inline std::vector<FormatCase> formatCases()
    {
        using C = PixelLayout::Components;
        using T = PixelLayout::Type;
        const PixelLayout rgba8 = layoutOf(C::RGBA, T::U8);
        std::vector<FormatCase> cases;
        const std::vector<U8> rgba = pattern(16, 1);
        const std::vector<U8> rgb = pattern(12, 2);
        cases.push_back({ "RGBA8 identity", descOf(Format::RGBA8Unorm, 0, 2, 2), rgba8, rgba, rgba8, rgba });
        cases.push_back({ "sRGB identity", descOf(Format::RGBA8Unorm_sRGB, 0, 2, 2), rgba8, rgba, rgba8, rgba });
        cases.push_back({ "RGB into GL_RGB8 has alpha 0xFF", descOf(Format::RGBA8Unorm, GL_FORMAT_RGB8, 2, 2),
                          layoutOf(C::RGB, T::U8), rgb, rgba8, withAlpha(rgb, 1, { 0xFF }) });
        cases.push_back({ "RGB into GL_RGBA8 has alpha 0xFF", descOf(Format::RGBA8Unorm, 0, 2, 2), layoutOf(C::RGB, T::U8),
                          rgb, rgba8, withAlpha(rgb, 1, { 0xFF }) });
        cases.push_back({ "RGB into GL_SRGB8 has alpha 0xFF", descOf(Format::RGBA8Unorm_sRGB, GL_FORMAT_SRGB8, 2, 2),
                          layoutOf(C::RGB, T::U8), rgb, rgba8, withAlpha(rgb, 1, { 0xFF }) });
        // CEF's and LibVLC's textures (P2-02c): BGRA bytes with alpha 0x00 into GL_RGB.
        const std::vector<U8> bgraZero = withAlpha(rgb, 1, { 0x00 });
        cases.push_back({ "BGRA with alpha 0x00 into GL_RGB has alpha 0xFF", descOf(Format::RGBA8Unorm, GL_FORMAT_RGB, 2, 2),
                          layoutOf(C::BGRA, T::U8), bgraZero, rgba8, redBlueSwapped(withAlpha(rgb, 1, { 0xFF })) });
        cases.push_back({ "BGRA into RGBA8 keeps alpha", descOf(Format::RGBA8Unorm, 0, 2, 2), layoutOf(C::BGRA, T::U8), rgba,
                          rgba8, redBlueSwapped(rgba) });
        cases.push_back({ "RGBA with 8_8_8_8_REV is the bytes", descOf(Format::RGBA8Unorm, 0, 2, 2),
                          layoutOf(C::RGBA, T::U32_8_8_8_8_Rev), rgba, rgba8, rgba });
        cases.push_back({ "BGRA with 8_8_8_8_REV is BGRA's bytes", descOf(Format::RGBA8Unorm, 0, 2, 2),
                          layoutOf(C::BGRA, T::U32_8_8_8_8_Rev), rgba, rgba8, redBlueSwapped(rgba) });
        cases.push_back({ "8_8_8_8_REV with swapBytes swaps the word", descOf(Format::RGBA8Unorm, 0, 2, 2),
                          layoutOf(C::RGBA, T::U32_8_8_8_8_Rev, true), rgba, rgba8, swapped(rgba, 4) });
        cases.push_back({ "rowLength 3", descOf(Format::RGBA8Unorm, 0, 2, 2), layoutOf(C::RGBA, T::U8, false, 3),
                          pattern(20, 3), rgba8, {} });
        {
            const std::vector<U8> source = cases.back().input;
            std::vector<U8>& expected = cases.back().expected;
            expected.assign(source.begin(), source.begin() + 8);
            expected.insert(expected.end(), source.begin() + 12, source.begin() + 20);
        }
        const std::vector<U8> rgba16 = pattern(32, 4);
        const PixelLayout rgba16Layout = layoutOf(C::RGBA, T::U16);
        cases.push_back({ "RGBA16Unorm identity", descOf(Format::RGBA16Unorm, 0, 2, 2), rgba16Layout, rgba16, rgba16Layout,
                          rgba16 });
        cases.push_back({ "RGBA16Unorm with swapBytes", descOf(Format::RGBA16Unorm, 0, 2, 2),
                          layoutOf(C::RGBA, T::U16, true), rgba16, rgba16Layout, swapped(rgba16, 2) });
        const std::vector<U8> rgb16 = pattern(24, 5);
        cases.push_back({ "RGB U16 into RGBA16Unorm has alpha 0xFFFF", descOf(Format::RGBA16Unorm, 0, 2, 2),
                          layoutOf(C::RGB, T::U16), rgb16, rgba16Layout, withAlpha(rgb16, 2, { 0xFF, 0xFF }) });
        const PixelLayout r32f = layoutOf(C::R, T::F32);
        const std::vector<U8> floats = floatBytes({ 0.25f, -1.5f, 3.f, 1e-3f });
        cases.push_back({ "R32F identity", descOf(Format::R32Float, 0, 2, 2), r32f, floats, r32f, floats });
        cases.push_back({ "R32F with swapBytes", descOf(Format::R32Float, 0, 2, 2), layoutOf(C::R, T::F32, true),
                          floats, r32f, swapped(floats, 4) });
        const std::vector<U8> rgb32 = floatBytes({ 0.f, 0.5f, 1.f, 2.f, -2.f, 0.125f, 7.f, 8.f, 9.f, 1e5f, -1e-5f, 0.75f });
        cases.push_back({ "RGB F32 into GL_RGB32F has alpha 1", descOf(Format::RGBA32Float, GL_FORMAT_RGB32F, 2, 2),
                          layoutOf(C::RGB, T::F32), rgb32, layoutOf(C::RGBA, T::F32),
                          withAlpha(rgb32, 4, floatBytes({ 1.f })) });
        const PixelLayout rgba16f = layoutOf(C::RGBA, T::F16);
        for (U32 map = 0; map < 2; ++map)
        {
            // P2-11a's two noise maps: GL_RGB and GL_FLOAT into GL_RGB16F.
            FormatCase noisy{ "RGB F32 noise map " + std::to_string(map + 1) + " into GL_RGB16F",
                              descOf(Format::RGBA16Float, GL_FORMAT_RGB16F, 4, 4), layoutOf(C::RGB, T::F32), {}, rgba16f, {} };
            noise(17 + map * 1000, 16, noisy.input, noisy.expected);
            cases.push_back(noisy);
        }
        // GL truncates (by a run-time probe): ties as 1 + 2^-11 and 1 + 3 * 2^-11 go down, F16's
        // subnormals below 2^-14 stay, +0 and -0 stay, 2 - 2^-11 does not round up over an exponent.
        cases.push_back({ "RGB F32 constructed into GL_RGB16F", descOf(Format::RGBA16Float, GL_FORMAT_RGB16F, 2, 2),
                          layoutOf(C::RGB, T::F32),
                          floatBytes({ 1.f + 0x1p-11f, 1.f + 3 * 0x1p-11f, 2.f - 0x1p-11f, 0x1p-15f, 0x1p-24f, 0x1p-25f,
                                       3 * 0x1p-25f, 0.f, -0.f, -(1.f + 3 * 0x1p-11f), 0.75f, 1e-8f }),
                          rgba16f,
                          wordBytes({ 0x3C00, 0x3C01, 0x3FFF, 0x3C00, 0x0200, 0x0001, 0x0000, 0x3C00, 0x0001, 0x0000,
                                      0x8000, 0x3C00, 0xBC01, 0x3A00, 0x0000, 0x3C00 }) });
        // Above 65504, the largest half: 65504 while the exponent is a half's, Inf above it.
        cases.push_back({ "RGB F32 above 65504 into GL_RGB16F", descOf(Format::RGBA16Float, GL_FORMAT_RGB16F, 2, 1),
                          layoutOf(C::RGB, T::F32), floatBytes({ 65504.f, 65519.f, 65520.f, 70000.f, -65520.f, 1e10f }),
                          rgba16f, wordBytes({ 0x7BFF, 0x7BFF, 0x7BFF, 0x3C00, 0x7C00, 0xFBFF, 0x7C00, 0x3C00 }) });
        cases.push_back({ "R F32 into R16Float", descOf(Format::R16Float, 0, 2, 2), r32f,
                          floatBytes({ 0.1f, 0.5f, 1.f + 0x1p-11f, 2.f }), layoutOf(C::R, T::F16),
                          wordBytes({ 0x2E66, 0x3800, 0x3C00, 0x4000 }) });
        const std::vector<U8> halves = wordBytes({ 0x3C00, 0xBC00, 0x0001, 0x7BFF });
        cases.push_back({ "R16Float identity", descOf(Format::R16Float, 0, 2, 2), layoutOf(C::R, T::F16), halves,
                          layoutOf(C::R, T::F16), halves });
        // Luminance and alpha as R or RG into swizzled R8 and RG8 (P2-02b).
        const PixelLayout r8 = layoutOf(C::R, T::U8);
        const PixelLayout rg8 = layoutOf(C::RG, T::U8);
        const std::vector<U8> lum = pattern(4, 6);
        const std::vector<U8> lumAlpha = pattern(8, 7);
        cases.push_back({ "GL_LUMINANCE as R into a swizzled R8",
                          descOf(Format::R8Unorm, 0, 2, 2, { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::One }), r8, lum, r8,
                          lum });
        cases.push_back({ "GL_ALPHA as R into a swizzled R8",
                          descOf(Format::R8Unorm, 0, 2, 2, { Swizzle::Zero, Swizzle::Zero, Swizzle::Zero, Swizzle::R }), r8,
                          lum, r8, lum });
        cases.push_back({ "GL_LUMINANCE_ALPHA as RG into a swizzled RG8",
                          descOf(Format::RG8Unorm, 0, 2, 2, { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::G }), rg8,
                          lumAlpha, rg8, lumAlpha });
        // Depth in [0, 1] (decision 7).
        const PixelLayout depth = layoutOf(C::Depth, T::F32);
        const std::vector<U8> depths = floatBytes({ 0.f, 0.25f, 0.5f, 1.f });
        cases.push_back({ "Depth32Float", descOf(Format::Depth32Float, 0, 2, 2), depth, depths, depth, depths });
        cases.push_back({ "Depth32Float_Stencil8's depth", descOf(Format::Depth32Float_Stencil8, 0, 2, 2), depth, depths,
                          depth, depths });
        // GL's generic compressed formats, uncompressed on Metal (P2-02a); GL counts its own sizes.
        const std::vector<U8> rgba44 = pattern(64, 8);
        const std::vector<U8> rgb44 = pattern(48, 9);
        cases.push_back({ "GL_COMPRESSED_RGBA", descOf(Format::RGBA8Unorm, GL_FORMAT_COMPRESSED_RGBA, 4, 4), rgba8,
                          rgba44, rgba8, rgba44, Scope::MetalOnly, 64 });
        cases.push_back({ "GL_COMPRESSED_SRGB_ALPHA", descOf(Format::RGBA8Unorm_sRGB, GL_FORMAT_COMPRESSED_SRGB_ALPHA, 4, 4),
                          rgba8, rgba44, rgba8, rgba44, Scope::MetalOnly, 64 });
        cases.push_back({ "GL_COMPRESSED_RGB has alpha 0xFF", descOf(Format::RGBA8Unorm, GL_FORMAT_COMPRESSED_RGB, 4, 4),
                          layoutOf(C::RGB, T::U8), rgb44, rgba8, withAlpha(rgb44, 1, { 0xFF }), Scope::MetalOnly, 48 });
        cases.push_back({ "GL_COMPRESSED_RED", descOf(Format::R8Unorm, GL_FORMAT_COMPRESSED_RED, 4, 4), r8, pattern(16, 10),
                          r8, pattern(16, 10), Scope::MetalOnly, 16 });
        cases.push_back({ "GL_COMPRESSED_RG", descOf(Format::RG8Unorm, GL_FORMAT_COMPRESSED_RG, 4, 4), rg8, pattern(32, 11),
                          rg8, pattern(32, 11), Scope::MetalOnly, 32 });
        return cases;
    }
}

// P3-08b S2: copies between formats through Encoder::copyTexture, one table for both backends as above. llrhigl_test.cpp's
// 'LLRHI GL transfers' test<40> sends each row through the GL device, whose glCopyTexSubImage2D gives the bytes (the
// oracle on this Mac), and llrhimetal_metal.mm's t191 through a Metal device, whose copy as a draw must give them: exact
// for the unorm source, within 1 for the float one (the Metal standard, decision 7).
namespace LL::RHI::CopyCases
{
    constexpr U32 SIDE = 4;                       ///< every texture is SIDE x SIDE with one level
    constexpr Region REGION{ 1, 1, 3, 2 };        ///< the source's region: off the origin, so a shift shows
    constexpr U32 DST_X = 0;                      ///< where the region goes, other rows than its own, so a flip shows
    constexpr U32 DST_Y = 2;
    constexpr U8 UNTOUCHED = 0xEE;                ///< every stored byte of a destination before the copy

    /// The RGBA8 source: every texel and component differs, and y weighs more than x, so the rows are not symmetric.
    inline std::vector<U8> sourceRGBA8()
    {
        std::vector<U8> bytes(SIDE * SIDE * 4);
        for (U32 y = 0; y < SIDE; ++y)
        {
            for (U32 x = 0; x < SIDE; ++x)
            {
                U8* texel = &bytes[(y * SIDE + x) * 4];
                texel[0] = U8(10 + 16 * x + 4 * y);
                texel[1] = U8(100 + 3 * x + 20 * y);
                texel[2] = U8(200 - 7 * x - 11 * y);
                texel[3] = U8(40 + 2 * x + 50 * y);
            }
        }
        return bytes;
    }

    struct Row
    {
        const char* name;
        TextureDesc desc;
        PixelLayout::Components read; ///< the destination is read back in its stored components
        U32 kept;                     ///< the source's components the destination stores: R, RG or RGBA
        bool alphaOne;                ///< a GL format without alpha reads alpha 1 (0xFF)
    };

    /// The destinations of the RGBA8 source. GL's copy into a swizzled R8 or RG8 takes the framebuffer's R (and G), so
    /// an alpha (A8) or luminance-alpha (LA) image loses the source's alpha on GL too; Metal copies GL, loss included
    /// (design.md's risk "GL loses LA alpha too").
    inline std::vector<Row> rows()
    {
        const auto descOf = [](Format format, U32 glFormat, SwizzleRGBA swizzle)
        {
            TextureDesc desc;
            desc.format = format;
            desc.glFormat = glFormat;
            desc.width = SIDE;
            desc.height = SIDE;
            desc.swizzle = swizzle;
            desc.usage = TEXTURE_SAMPLED | TEXTURE_READBACK;
            return desc;
        };
        using C = PixelLayout::Components;
        return {
            { "R8", descOf(Format::R8Unorm, 0, {}), C::R, 1, false },
            { "RG8", descOf(Format::RG8Unorm, 0, {}), C::RG, 2, false },
            { "RGB8", descOf(Format::RGBA8Unorm, FormatCases::GL_FORMAT_RGB8, {}), C::RGBA, 4, true },
            { "A8 (a swizzled R8)", descOf(Format::R8Unorm, 0, { Swizzle::Zero, Swizzle::Zero, Swizzle::Zero, Swizzle::R }),
              C::R, 1, false },
            { "LA (a swizzled RG8)", descOf(Format::RG8Unorm, 0, { Swizzle::R, Swizzle::R, Swizzle::R, Swizzle::G }), C::RG,
              2, false },
        };
    }

    /// The destination's stored bytes before the copy, in its read components.
    inline std::vector<U8> untouched(const Row& row)
    {
        const U32 stored = row.read == PixelLayout::Components::RGBA ? 4 : row.kept;
        return std::vector<U8>(SIDE * SIDE * stored, UNTOUCHED);
    }

    /// The destination's bytes after the copy of REGION to (DST_X, DST_Y), as read back in its components.
    inline std::vector<U8> expected(const Row& row)
    {
        const U32 stored = row.read == PixelLayout::Components::RGBA ? 4 : row.kept;
        const std::vector<U8> source = sourceRGBA8();
        std::vector<U8> bytes(SIDE * SIDE * stored, UNTOUCHED);
        for (U32 j = 0; j < REGION.height; ++j)
        {
            for (U32 i = 0; i < REGION.width; ++i)
            {
                const U8* from = &source[((REGION.y + j) * SIDE + REGION.x + i) * 4];
                U8* to = &bytes[((DST_Y + j) * SIDE + DST_X + i) * stored];
                for (U32 c = 0; c < row.kept; ++c)
                {
                    to[c] = from[c];
                }
            }
        }
        if (row.alphaOne)
        {
            for (U32 texel = 0; texel < SIDE * SIDE; ++texel)
            {
                bytes[texel * 4 + 3] = 0xFF;
            }
        }
        return bytes;
    }

    /// The float row: a whole RGBA16F source into an RGBA8 destination. The halves are exact and none is half a step
    /// from a byte, so GL's round to nearest gives one byte each: below 0 and above 1 clamp.
    constexpr U16 HALVES[8] = { 0xBC00, 0x0000, 0x3000, 0x3400, 0x3600, 0x3A00, 0x3B00, 0x4000 }; ///< -1 0 1/8 1/4 3/8 3/4 7/8 2
    constexpr U8 HALF_BYTES[8] = { 0, 0, 32, 64, 96, 191, 223, 255 };                            ///< round(clamp(h) * 255)

    /// Component c of texel t takes HALVES[(t * 3 + c) % 8], so each value is in every channel.
    inline std::vector<U8> sourceRGBA16F()
    {
        std::vector<U8> bytes(SIDE * SIDE * 4 * 2);
        for (U32 t = 0; t < SIDE * SIDE; ++t)
        {
            for (U32 c = 0; c < 4; ++c)
            {
                const U16 half = HALVES[(t * 3 + c) % 8];
                std::memcpy(&bytes[(t * 4 + c) * 2], &half, 2);
            }
        }
        return bytes;
    }

    inline std::vector<U8> expectedFromRGBA16F()
    {
        std::vector<U8> bytes(SIDE * SIDE * 4);
        for (U32 t = 0; t < SIDE * SIDE; ++t)
        {
            for (U32 c = 0; c < 4; ++c)
            {
                bytes[t * 4 + c] = HALF_BYTES[(t * 3 + c) % 8];
            }
        }
        return bytes;
    }
}

#endif // LL_LLRHIFORMATCASES_H
