/**
 * @file luminousbenchmark.h
 * @brief A fixed benchmark at Blake Sea - Windlass, to compare Macs.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
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
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMINOUS_BENCHMARK_H
#define LUMINOUS_BENCHMARK_H

/**
 * The same view on every Mac: in the region Blake Sea - Windlass, the camera
 * at a fixed place looking at a fixed point, the sun fixed, every avatar's
 * animation held still, the frame-rate cap and the background yield ignored
 * while it runs (llappviewer.cpp asks running()).
 * It measures the live view with each effect as set, then with every effect
 * at low quality, then times a 16-frame render at the window's size, and
 * puts everything back.
 * Results are shown and added to user_settings/luminous_benchmark.txt with
 * the GPU, the CPU and the window's size -- the window's size matters most
 * when comparing two Macs.
 */
namespace LuminousBenchmark
{
    /// Registers Luminous.Benchmark, which starts one.
    void init();
    bool running();
}

#endif // LUMINOUS_BENCHMARK_H
