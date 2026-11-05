/**
 * @file llrhimetal_test.cpp
 * @brief The Metal backend on this Mac, under Metal API Validation (P3-01a)
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

#include "linden_common.h"

#include "../llrhi.h"
#include "../llrhimetal.h"

#include "../test/lltut.h"

#include <cstdlib>
#include <string>

// In llrhimetal_metal.mm: the Metal device's tests, which read Metal's objects. Each makes its own
// device, and passes with the reason in the log when the build or the Mac gives GL.
void runMetalTest(int test);
// In llrhimetal_metal.mm: runs a test with stderr in a file and fails it on a validation line
// there, since a shader validation report neither ends the program nor fails the commit.
void runValidated(void (*test)());

// The Metal backend's real code on this Mac. run_build_test.py sets Metal API Validation and shader
// validation in the environment before the test starts (LL_ADD_INTEGRATION_TEST in
// llrender/CMakeLists.txt), so every Metal call here is validated.

namespace tut
{
    struct rhimetal_data
    {
    };
    // t44b is test 70; t128 is test 130; t54b is test 131; t129 is test 132; t131, t134, t136 and t138 are tests 133-136;
    // t130, t132 and t133 are tests 137-139; t140 is test 140; t137 is test 141; t141 is test 142; t135 and t139 are tests
    // 143 and 144; t142 is test 145; t143 is test 146; t145 and t146 are
    // tests 147 and 148; t147 is test 149; t150 is test 150 (P2-06a); t148 is test 151 (P2-16b); t151, t152 and t153 are
    // tests 152-154 (P2-07b and P2-27); t155 is test 155 (P2-27); t156 is test 156 (P2-R2); t157-t159 are tests 157-159
    // (P3-02); t160 is test 160 (P3-01h); t161 and t162 are tests 161 and 162 (P3-05a1); t163 and t164 are tests 163 and
    // 164 (P3-04a); t165 is test 165 (P3-01i, its t163 before the batch merge); t170-t176 are tests 170-176 (P3-03a, from
    // 170 so the units beside it keep theirs); t177-t179 are tests 177-179 (P3-03c); t180-t183 are tests 180-183 (P3-08's
    // occlusion part, from 180 likewise); t184 is test 184 (P3-03c's review m1, its t180 before batch 2's merge); t185 is
    // test 185 (P3-09 S1); t186 and t189 are tests 186 and 189 (P3-04c, t189 its t185 before batch 4's merge); t187 and
    // t188 are tests 187 and 188 (P3-01k, its t184 and t185 before batch 3's merge; its review M1); t190-t192 are tests
    // 190-192 (P3-08b, a block of its own to 194); t195-t198 are tests 195-198 (P3-08 lane B); t200-t206 are tests
    // 200-206 (P3-04b, from 200 likewise); t210-t212 are tests 210-212 (P3-09b, from 210 likewise); t213 and t214 are tests
    // 213 and 214 (P3-R4); t215-t217 are tests 215-217; t231 is test 231 (vsync-present2)
    typedef test_group<rhimetal_data, 231> rhimetal_group;
    typedef rhimetal_group::object object;
    rhimetal_group rhimetal_test_group("Metal backend");

    template<> template<>
    void object::test<1>()
    {
        set_test_name("Metal API Validation is on");
        const char* layer = getenv("MTL_DEBUG_LAYER");
        ensure("MTL_DEBUG_LAYER is 1", layer && std::string(layer) == "1");
    }

    template<> template<>
    void object::test<2>()
    {
        set_test_name("the startup check on this Mac gives Metal exactly when there is no reason");
        // The first Metal device of the process, so validation's banners come here.
        runValidated([]()
        {
            const LL::RHI::BackendChoice choice = LL::RHI::chooseBackend(LL::RHI::BackendType::Metal);
            ensure_equals("Metal exactly when the reason is empty", choice.type == LL::RHI::BackendType::Metal,
                          choice.reason.empty());
            if (choice.type == LL::RHI::BackendType::GL)
            {
                // A Mac without Metal 4 passes too; the reason is in the test's log.
                LL_INFOS("RHI") << "This Mac gives GL: " << choice.reason << LL_ENDL;
            }
        });
    }

    template<> template<>
    void object::test<3>()
    {
        set_test_name("the Metal device: backend, capabilities and the first frame");
        runMetalTest(1);
    }

    template<> template<>
    void object::test<4>()
    {
        set_test_name("textures: RGBA8Unorm and Depth32Float are private with their usage; the rest is refused");
        runMetalTest(2);
    }

    template<> template<>
    void object::test<5>()
    {
        set_test_name("residency: a released texture leaves the set only after its frame has finished");
        runMetalTest(3);
    }

    template<> template<>
    void object::test<6>()
    {
        set_test_name("frames: finished frames come from the events, at most 3 in flight, every commit reported");
        runMetalTest(4);
    }

    template<> template<>
    void object::test<7>()
    {
        set_test_name("a device destroyed right after a submit drains its feedback without a WARN line");
        runMetalTest(5);
    }

    template<> template<>
    void object::test<8>()
    {
        set_test_name("a clear at the pass's start becomes its color load action");
        runMetalTest(6);
    }

    template<> template<>
    void object::test<9>()
    {
        set_test_name("the clear's depth becomes a load action only when the bound state writes depth");
        runMetalTest(7);
    }

    template<> template<>
    void object::test<10>()
    {
        set_test_name("a clear inside a scissor is a draw of the clear program, with no WARN line");
        runMetalTest(8);
    }

    template<> template<>
    void object::test<11>()
    {
        set_test_name("metalWritesDepth and the depth state's Metal description");
        runMetalTest(9);
    }

    template<> template<>
    void object::test<12>()
    {
        set_test_name("t1: a clear read back after its pass gives the clear color in GL's bytes");
        runMetalTest(10);
    }

    template<> template<>
    void object::test<13>()
    {
        set_test_name("t2: a clear at the start of a pass that loads is read back; a readback before a clear does not see it");
        runMetalTest(11);
    }

    template<> template<>
    void object::test<14>()
    {
        set_test_name("t3: a readback waited for inside its pass, under a debug group, without a WARN line");
        runMetalTest(12);
    }

    template<> template<>
    void object::test<15>()
    {
        set_test_name("t3b: the same with the pass's DontCare: the bytes are still the color");
        runMetalTest(13);
    }

    template<> template<>
    void object::test<16>()
    {
        set_test_name("t4: row order and region against an oracle filled through a Metal 3 queue");
        runMetalTest(14);
    }

    template<> template<>
    void object::test<17>()
    {
        set_test_name("t5: an unsupported layout or source gives null and one WARN line");
        runMetalTest(15);
    }

    template<> template<>
    void object::test<18>()
    {
        set_test_name("t6: frames of readbacks, waited before and after sending, dropped unwaited, submitAndWait");
        runMetalTest(16);
    }

    template<> template<>
    void object::test<19>()
    {
        set_test_name("t7: a failed command buffer leaves the readback without data, and wait() WARNs once (D21)");
        runMetalTest(17);
    }

    template<> template<>
    void object::test<20>()
    {
        set_test_name("t8: a submitAndWait that times out keeps the released resources resident");
        runMetalTest(18);
    }

    template<> template<>
    void object::test<21>()
    {
        set_test_name("t19: every Format but Unknown has a Metal format in the table");
        runMetalTest(19);
    }

    template<> template<>
    void object::test<22>()
    {
        set_test_name("t20: a buffer is Shared with its bytes, resident until its frame has finished; size 0 and dynamic are refused");
        runMetalTest(20);
    }

    template<> template<>
    void object::test<23>()
    {
        set_test_name("t21: an upload outside a pass is read back in order on its own encoder; one in a pass breaks it and is taken; BGRA and depth uploads are taken");
        runMetalTest(21);
    }

    template<> template<>
    void object::test<24>()
    {
        set_test_name("t22: point samplers with clamp are cached with a gpuResourceID, a comparing one is refused; the device lets go of them before its releases");
        runMetalTest(22);
    }

    template<> template<>
    void object::test<25>()
    {
        set_test_name("t23: the UI program is Ready with its reflection, attribute mask and layout from the translation");
        runMetalTest(23);
    }

    template<> template<>
    void object::test<26>()
    {
        set_test_name("t24: the packing program's places are the translation's, and a uniform in both stages has one index");
        runMetalTest(24);
    }

    template<> template<>
    void object::test<27>()
    {
        set_test_name("t25: a syntax error, a uniform block, a geometry stage, no translator and MSL that does not compile each fail with one WARN line");
        runMetalTest(25);
    }

    template<> template<>
    void object::test<28>()
    {
        set_test_name("t26: MSL 4.0 with the math constants, and the INFO line once per device");
        runMetalTest(26);
    }

    template<> template<>
    void object::test<29>()
    {
        set_test_name("t27: the UI pipeline is cached, the vertex table is LLVertexBuffer's, what is not ported is refused, and a dying program's pipelines go");
        runMetalTest(27);
    }

    template<> template<>
    void object::test<30>()
    {
        set_test_name("t28: one draw of a triangle with red, green and blue corners and a yellow square, then its program and pipeline go in the frame");
        runMetalTest(28);
    }

    template<> template<>
    void object::test<31>()
    {
        set_test_name("t29: a viewport and a scissor in GL's numbers color exactly their rows and columns, also on an open encoder");
        runMetalTest(29);
    }

    template<> template<>
    void object::test<32>()
    {
        set_test_name("t30: each draw takes the blocks as they are, a program keeps its values, arrays and matrix columns go to their places, and both stages' blocks reach the GPU");
        runMetalTest(30);
    }

    template<> template<>
    void object::test<33>()
    {
        set_test_name("t31: a readback in the pass has only the draw before it, and only the last segment has the pass's store actions");
        runMetalTest(31);
    }

    template<> template<>
    void object::test<34>()
    {
        set_test_name("t32: points and missing streams each give one WARN line and no draw");
        runMetalTest(32);
    }

    template<> template<>
    void object::test<35>()
    {
        set_test_name("t33: the texture index stream is the position's fourth component, as LLVertexBuffer binds it");
        runMetalTest(33);
    }

    template<> template<>
    void object::test<36>()
    {
        set_test_name("t34: texture storage and usage per usage row, Private and never memoryless, and the new formats, swizzle, cube and cube array");
        runMetalTest(34);
    }

    template<> template<>
    void object::test<37>()
    {
        set_test_name("t35: the shared format cases on Metal give exactly GL's bytes");
        runMetalTest(35);
    }

    template<> template<>
    void object::test<38>()
    {
        set_test_name("t36: cube faces, cube array layers, levels, a region with a row length and generateMipmaps");
        runMetalTest(36);
    }

    template<> template<>
    void object::test<39>()
    {
        set_test_name("t37: copyTexture within a format, between formats and in a pass, and generateMipmaps in a pass");
        runMetalTest(37);
    }

    template<> template<>
    void object::test<40>()
    {
        set_test_name("t38: a readback in a layout that is not the format's texels gives null and one WARN line");
        runMetalTest(38);
    }

    template<> template<>
    void object::test<41>()
    {
        set_test_name("t39: with the Metal debug setting a draw of an allocation outside the residency set is refused with its name; without it the check is skipped");
        runMetalTest(39);
    }

    template<> template<>
    void object::test<42>()
    {
        set_test_name("t40: a draw past the end of a vertex stream is refused with the buffer's name; one to the last vertex is drawn");
        runMetalTest(40);
    }

    template<> template<>
    void object::test<43>()
    {
        set_test_name("t41: metalCheckIndexRange names indices past the buffer and index values past the vertices");
        runMetalTest(41);
    }

    template<> template<>
    void object::test<44>()
    {
        set_test_name("t42: a buffer of each usage is Shared with the default cache mode and has its bytes from creation; dynamic ones are pool blocks up to 64 KiB; size 0 is refused");
        runMetalTest(42);
    }

    template<> template<>
    void object::test<45>()
    {
        set_test_name("t43: updates have GL's semantics: a dynamic update in a pass, a partial update, a static update outside a pass and its refusal inside one");
        runMetalTest(43);
    }

    template<> template<>
    void object::test<46>()
    {
        set_test_name("t44: with 3 frames in flight the fourth recording waits for the first frame, whose ring part is not overwritten");
        runMetalTest(44);
    }

    template<> template<>
    void object::test<47>()
    {
        set_test_name("t45: released buffers, ring overflow buffers and textures stay resident until their frame's event; a freed pool block is reused only after its frame");
        runMetalTest(45);
    }

    template<> template<>
    void object::test<48>()
    {
        set_test_name("t46: the churn of textures and dynamic updates twice ends with the same residency count and bounded memory and releases");
        runMetalTest(46);
    }

    template<> template<>
    void object::test<49>()
    {
        set_test_name("t47: LLVertexBuffer on the viewer's Metal device: the allocation is the CPU copy, a map sends the whole copy once, a re-size has new storage, index re-sizes keep getBytesAllocated() and residency right");
        runMetalTest(47);
    }

    template<> template<>
    void object::test<50>()
    {
        set_test_name("t48: createTextureNames on Metal draws no names and makes textures with storage; RGB/F32 reads back; createTextureWithData leaves the GL binding at 0");
        runMetalTest(48);
    }

    template<> template<>
    void object::test<51>()
    {
        set_test_name("t49: respecifyEmptyTexture on Metal is a new texture at the new size, the old leaves the residency set after its frame; size 0 and over the maximum keep it");
        runMetalTest(49);
    }

    template<> template<>
    void object::test<52>()
    {
        set_test_name("t50: LLImageGL's accounting on Metal is keyed by the texture's id and counts GL's sizes, also GL_COMPRESSED_RGBA; a re-specification moves the bytes");
        runMetalTest(50);
    }

    template<> template<>
    void object::test<53>()
    {
        set_test_name("t51: LLCubeMap with one level on Metal: one Cube texture without GL names, six face uploads to their layers, generateMipMaps without WARN");
        runMetalTest(51);
    }

    template<> template<>
    void object::test<54>()
    {
        set_test_name("t52: allocateScreenBufferInternal's LLRenderTarget sequence on the viewer's Metal device, without and with HDR: deferredScreen's pass has its depth, the clears read back, screen's shared depth and a depth-only target clear, no LL_ERRS or WARN");
        runMetalTest(52);
    }

    template<> template<>
    void object::test<55>()
    {
        set_test_name("t53: LLRenderTarget::resize on Metal gives every attachment a new texture at the new size, the old leave the residency set after the frame, the sharer uses the new depth, and the byte counts come back");
        runMetalTest(53);
    }

    template<> template<>
    void object::test<56>()
    {
        set_test_name("t54: a 2D LLImageGL from createGLTexture on the viewer's Metal device has its texture without a GL name, and setSubImage uploads a region and the whole image to it, no WARN");
        runMetalTest(54);
    }

    template<> template<>
    void object::test<57>()
    {
        set_test_name("t55: a uniform no stage reads is not in Metal's reflection, as GL gives it no location, and its index is the facade's no such uniform");
        runMetalTest(55);
    }

    template<> template<>
    void object::test<58>()
    {
        set_test_name("t56: ProgramDesc::uniformBlocks gives Metal's reflection the blocks with name and size and each stage its slots' bits; another block fails with one WARN");
        runMetalTest(56);
    }

    template<> template<>
    void object::test<59>()
    {
        set_test_name("t57: a stage with 17 textures and samplers, or a block in buffer 8, fails at createProgram with one WARN; 16 textures are Ready");
        runMetalTest(57);
    }

    template<> template<>
    void object::test<60>()
    {
        set_test_name("t58: an array read as a[1] in the vertex stage and a[4] in the fragment stage has arraySize 5");
        runMetalTest(58);
    }

    template<> template<>
    void object::test<61>()
    {
        set_test_name("t59: with the pool paused, a login screen program made after three others is translated on the creating thread inside createProgram and is Ready without wait() while they are Pending; resumed, all are Ready");
        runMetalTest(59);
    }

    template<> template<>
    void object::test<62>()
    {
        set_test_name("t60: wait() on a program in the paused pool's queue takes it, translates it on the waiting thread and is Ready within 5 s");
        runMetalTest(60);
    }

    template<> template<>
    void object::test<63>()
    {
        set_test_name("t61: createProgram copies the description: its strings and the uniform blocks' names are overwritten after it returns, and the program is Ready with the right reflection");
        runMetalTest(61);
    }

    template<> template<>
    void object::test<64>()
    {
        set_test_name("t62: the translator is the one createProgram saw: a broken one set back before the pool takes the job still fails it");
        runMetalTest(62);
    }

    template<> template<>
    void object::test<65>()
    {
        set_test_name("t63: the Metal backend's code and the translator's include and use nothing of LLGLSLShader, LLShaderMgr or the settings; a scratch copy with LLShaderMgr::instance() is caught");
        runMetalTest(63);
    }

    template<> template<>
    void object::test<66>()
    {
        set_test_name("t64: a fragment stage that does not compile, a geometry stage and a vertex library Metal refuses are Failed after wait() with one WARN line each, the last with Metal's text");
        runMetalTest(64);
    }

    template<> template<>
    void object::test<67>()
    {
        set_test_name("t65: a released program in the queue is never translated, and one released while its libraries compile drops the result without a WARN line");
        runMetalTest(65);
    }

    template<> template<>
    void object::test<68>()
    {
        set_test_name("t66: ~MetalDevice with jobs queued and a library handler asleep returns after every handler has reported, and none reports after it");
        runMetalTest(66);
    }

    template<> template<>
    void object::test<69>()
    {
        set_test_name("t67: the UI and the packing programs' MSL compile as one library, each stage in a namespace of its own, and v0::main0 to f1::main0 resolve into pipelines (flag A's probe)");
        runMetalTest(67);
    }

    template<> template<>
    void object::test<70>()
    {
        set_test_name("t44b: the ring is rewound at the frame's end, so a frame's first draw or upload keeps its ring bytes");
        runMetalTest(144);
    }

    template<> template<>
    void object::test<71>()
    {
        set_test_name("t69: 20 programs made twice in the same order on new devices, with other timing in the pool, give the same groups of 16 and 4");
        runMetalTest(69);
    }

    template<> template<>
    void object::test<72>()
    {
        set_test_name("t70: wait() on member 3 of an open group closes it with members 0-3, which are Ready, and the next program opens a new group");
        runMetalTest(70);
    }

    template<> template<>
    void object::test<73>()
    {
        set_test_name("t71: an open group of 5 is not sent before endFrame closes it, and its members become Ready without wait()");
        runMetalTest(71);
    }

    template<> template<>
    void object::test<74>()
    {
        set_test_name("t72: a group with a member whose MSL does not compile falls back to single programs: the others are Ready, the broken one Failed with one WARN line");
        runMetalTest(72);
    }

    template<> template<>
    void object::test<75>()
    {
        set_test_name("t73: ~MetalDevice with a group compiling and a fall-back on its way returns after every handler has reported, and none reports after it");
        runMetalTest(73);
    }

    template<> template<>
    void object::test<76>()
    {
        set_test_name("t74: flag B: blending off is a full state sent Disabled that draws; blending on one RGBA8 attachment is a full state sent Enabled that draws; four attachments and R32Uint are Ready, R32Uint alone Disabled beside a blended one, under validation");
        runMetalTest(74);
    }

    template<> template<>
    void object::test<77>()
    {
        set_test_name("t75: two attribute masks that differ outside the program's, two depth formats and 12 formats x blend x masks give 12 full states shared by 48 cached pipelines");
        runMetalTest(75);
    }

    template<> template<>
    void object::test<78>()
    {
        set_test_name("t76: alpha and additive blending on RGBA8 give the CPU's bytes, and color mask R+A leaves G and B as cleared");
        runMetalTest(76);
    }

    template<> template<>
    void object::test<79>()
    {
        set_test_name("t77: getPipeline comes back at once with a Pending pipeline, a draw with it waits and draws, and a refused key gives one WARN line once");
        runMetalTest(77);
    }

    template<> template<>
    void object::test<80>()
    {
        set_test_name("t78: a program that goes takes its cached pipelines, its states and its refusals with it, and a reload makes new ones");
        runMetalTest(78);
    }

    template<> template<>
    void object::test<81>()
    {
        set_test_name("t79: points (since P3-01e S8), lines and 4 samples, blended or not, make Ready pipelines");
        runMetalTest(79);
    }

    template<> template<>
    void object::test<82>()
    {
        set_test_name("t80: ~MetalDevice with a pipeline compiling and two more sent after it returns after every handler has reported, and none reports after it");
        runMetalTest(80);
    }

    template<> template<>
    void object::test<83>()
    {
        set_test_name("t81: a block on slot 0 draws the uniform buffer's bytes at the bound offset; no buffer on the slot, or a range under the block's size, is refused with one WARN line naming the program and the slot; another slot leaves slot 0's range");
        runMetalTest(81);
    }

    template<> template<>
    void object::test<84>()
    {
        set_test_name("t82: each stage's default block goes to the ring once per frame unless a setter changes its bytes; the same bytes mark nothing and a changed value copies only its stage");
        runMetalTest(82);
    }

    template<> template<>
    void object::test<85>()
    {
        set_test_name("t83: texture0 on channel 1 and texture1 on channel 0, as uniform1i sets them, each sample their channel's texture");
        runMetalTest(83);
    }

    template<> template<>
    void object::test<86>()
    {
        set_test_name("t84: a program's uniform state on the encoder goes with the program");
        runMetalTest(84);
    }

    template<> template<>
    void object::test<87>()
    {
        set_test_name("t85: a draw chooses GL's sampler filters on Metal: Point, Bilinear, Trilinear, mips in effect, anisotropy 4");
        runMetalTest(85);
    }

    template<> template<>
    void object::test<88>()
    {
        set_test_name("t86: an unbound 2D, cube and cube array sampler reads the default texture's (0, 0, 0, 1); a shadow sampler reads 0 unbound and compares a bound Depth32Float (P3-02 D2), Never gives 0 where LessEqual passes (m7), and an RGBA8 texture on it is refused with a WARN (m6)");
        runMetalTest(86);
    }

    template<> template<>
    void object::test<89>()
    {
        set_test_name("t87: drawIndexed with 16- and 32-bit indices from a first index past 0, a dynamic buffer's current memory, and 16- then 32-bit on the same 12 and 6 bytes (the length rounded down)");
        runMetalTest(87);
    }

    template<> template<>
    void object::test<90>()
    {
        set_test_name("t88: draw(Triangles, 3, 3) draws the second triangle only");
        runMetalTest(88);
    }

    template<> template<>
    void object::test<91>()
    {
        set_test_name("t89: with the Metal debug setting drawIndexed checks the indices' CPU copy (a value past the vertices, an update's bytes) and the index buffer's residency, naming it; without the setting nothing is checked");
        runMetalTest(89);
    }

    template<> template<>
    void object::test<92>()
    {
        set_test_name("t90: an argument table place is written only when its value changes; each device's encoder has its own tables");
        runMetalTest(90);
    }

    template<> template<>
    void object::test<93>()
    {
        set_test_name("t91: depth writes in a read-only depth pass, a failed program, a refused shader and a pipeline that fails while the draw waits are refused with no native draw");
        runMetalTest(91);
    }

    template<> template<>
    void object::test<94>()
    {
        set_test_name("t92: Lines and LineStrip draw natively, indexed too; a strip is a list since S8");
        runMetalTest(92);
    }

    template<> template<>
    void object::test<95>()
    {
        set_test_name("t93: LLTexUnit's tell on the viewer's Metal device (16 channels) follows its record: Sent, Same, Same after the frame's end, a refused texture kept, Sent on another device");
        runMetalTest(93);
    }

    template<> template<>
    void object::test<96>()
    {
        set_test_name("t94: a static buffer updated between two draws in one pass: the first draw reads the old bytes, the second the new ones");
        runMetalTest(94);
    }

    template<> template<>
    void object::test<97>()
    {
        set_test_name("t95: a readback in the middle of an LLRenderTarget's stack pass has the first draw only, the target both, and currentPass's record survives the break");
        runMetalTest(95);
    }

    template<> template<>
    void object::test<98>()
    {
        set_test_name("t96: an upload in a pass after a draw: the draw before it samples the old texel, the draw after it the new one");
        runMetalTest(96);
    }

    template<> template<>
    void object::test<99>()
    {
        set_test_name("t97: copyTexture in a pass after a draw: the copy has the draw, and a draw after it samples the copy");
        runMetalTest(97);
    }

    template<> template<>
    void object::test<100>()
    {
        set_test_name("t98: one queue barrier per native render and compute encoder; an untouched stack pass opens no render encoder");
        runMetalTest(98);
    }

    template<> template<>
    void object::test<101>()
    {
        set_test_name("t99: a clear after a draw: color cleared in a new segment, depth kept, later draws on top");
        runMetalTest(99);
    }

    template<> template<>
    void object::test<102>()
    {
        set_test_name("t100: a clear inside a scissor changes only its rectangle, also with a smaller viewport and with culling");
        runMetalTest(100);
    }

    template<> template<>
    void object::test<103>()
    {
        set_test_name("t101: a clear with a pipeline that writes no alpha keeps the alpha");
        runMetalTest(101);
    }

    template<> template<>
    void object::test<104>()
    {
        set_test_name("t102: a scissored clear of R32Uint gives GLuint(rgba), of a depth-only pass the exact depth, and of two attachments both");
        runMetalTest(102);
    }

    template<> template<>
    void object::test<105>()
    {
        set_test_name("t103: with no translator registered a clear draw writes one WARN line and draws nothing");
        runMetalTest(103);
    }

    template<> template<>
    void object::test<106>()
    {
        set_test_name("t104: a clear as a load action and as a clear draw give the same bytes for four formats");
        runMetalTest(104);
    }

    template<> template<>
    void object::test<107>()
    {
        set_test_name("t105: a device destroyed right after a clear draw leaves no release unfreed");
        runMetalTest(105);
    }

    template<> template<>
    void object::test<108>()
    {
        set_test_name("t106: generateMipmaps makes each level with one bilinear tap of the level above, per format class and odd size");
        runMetalTest(106);
    }

    template<> template<>
    void object::test<109>()
    {
        set_test_name("t107: generateMipmaps gives each face of a cube and a cube array its own levels");
        runMetalTest(107);
    }

    template<> template<>
    void object::test<110>()
    {
        set_test_name("t108: generateMipmaps in a pass after a draw: the draw samples the old levels");
        runMetalTest(108);
    }

    template<> template<>
    void object::test<111>()
    {
        set_test_name("t109: generateMipmaps with no translator registered writes one WARN line and leaves the levels");
        runMetalTest(109);
    }

    template<> template<>
    void object::test<112>()
    {
        set_test_name("t110: a device destroyed right after generateMipmaps leaves no release unfreed");
        runMetalTest(110);
    }

    template<> template<>
    void object::test<113>()
    {
        set_test_name("t111: the price of a 1024x1024 RGBA8's mips");
        runMetalTest(111);
    }

    template<> template<>
    void object::test<114>()
    {
        set_test_name("t112: LLRenderTarget::clear clears the depth by the facade's write mask alone");
        runMetalTest(112);
    }

    template<> template<>
    void object::test<115>()
    {
        set_test_name("t113: an interface clear in a stack pass outside OwnGL keeps the depth under a state without writes");
        runMetalTest(113);
    }

    template<> template<>
    void object::test<116>()
    {
        set_test_name("t114: LLRenderTarget::clear writes the colours LLRender::setColorMask allows");
        runMetalTest(114);
    }

    template<> template<>
    void object::test<117>()
    {
        set_test_name("t115: LLRenderTarget::clear inside a scissor changes only its rectangle");
        runMetalTest(115);
    }

    template<> template<>
    void object::test<118>()
    {
        set_test_name("t116: endRenderPass on a stack pass outside OwnGL writes one WARN line");
        runMetalTest(116);
    }

    template<> template<>
    void object::test<119>()
    {
        set_test_name("t117: fans, strips and loops give the pixels of GL's triangles and lines, indexed or not");
        runMetalTest(117);
    }

    template<> template<>
    void object::test<120>()
    {
        set_test_name("t118: the lists of fans, strips and loops have GL's provoking vertex first, cached up to 64");
        runMetalTest(118);
    }

    template<> template<>
    void object::test<121>()
    {
        set_test_name("t119: points of size 1 and 4 through the point variant, made once per program");
        runMetalTest(119);
    }

    template<> template<>
    void object::test<122>()
    {
        set_test_name("t120: setLineWidth(3) records 1 without a WARN line, and lines stay one pixel");
        runMetalTest(120);
    }

    template<> template<>
    void object::test<123>()
    {
        set_test_name("t121: pipeline, encoder and command buffer labels read back; "
                      "a capture without MTL_CAPTURE_ENABLED warns once, in the frame asked for; "
                      "a device made while a capture is asked for has full pipeline states only");
        runMetalTest(121);
    }

    template<> template<>
    void object::test<124>()
    {
        set_test_name("t122: debug groups across a stack pass's begin and a pass break "
                      "stay off the command buffer while encoding");
        runMetalTest(122);
    }

    template<> template<>
    void object::test<125>()
    {
        set_test_name("t123: a device destroyed in a pass with open groups ends its encoder with store actions");
        runMetalTest(123);
    }

    template<> template<>
    void object::test<126>()
    {
        set_test_name("t124: a released texture or mip view leaves the argument table shadows, "
                      "so a texture given its ID writes its place");
        runMetalTest(124);
    }

    template<> template<>
    void object::test<127>()
    {
        set_test_name("t125: render targets without alpha in GL keep alpha 1 through draws, blending, clears, "
                      "copies and readbacks; a never written one loads as RGB 0");
        runMetalTest(125);
    }

    template<> template<>
    void object::test<128>()
    {
        set_test_name("t126: a cube's faces and texels as GL 4.6's table 8.19 gives them (flag F)");
        runMetalTest(126);
    }

    template<> template<>
    void object::test<129>()
    {
        set_test_name("t127: with the Metal debug setting a fan's generated list and its list in the ring are "
                      "resident before the draw, else refused by name");
        runMetalTest(127);
    }

    template<> template<>
    void object::test<130>()
    {
        set_test_name("t128: the mip encoders and the alpha after a copy take the open debug groups from the "
                      "command buffer and give them back");
        runMetalTest(128);
    }

    template<> template<>
    void object::test<131>()
    {
        set_test_name("t54b: LLImageGL's texture key on Metal is its own texture's id and follows it: a new createGLTexture, "
                      "setImage's and respecifyForScale's re-specifications, the cube's faces, 0 after destroyGLTexture "
                      "(P2-07f)");
        runMetalTest(154);
    }

    template<> template<>
    void object::test<132>()
    {
        set_test_name("t129: the scissor facade on Metal registers no pass and sends every call, also a repeated box and the "
                      "first in a new pass (P2-07f)");
        runMetalTest(129);
    }

    template<> template<>
    void object::test<133>()
    {
        set_test_name("t131: immediate mode through LLRender and an LLGLSLShader on Metal: a quad in window coordinates covers "
                      "exactly the pixels whose centres are inside, corners inside and just outside exact (P3-01f)");
        runMetalTest(131);
    }

    template<> template<>
    void object::test<134>()
    {
        set_test_name("t134: a 0x0 LLRenderTarget with a depth on Metal has no texture, no raw name and draws no names "
                      "(P3-01f, F-1 option 1)");
        runMetalTest(134);
    }

    template<> template<>
    void object::test<135>()
    {
        set_test_name("t136: scaleDown on Metal through the screen triangle: an 8x8 texture of exact levels, its bytes booked, "
                      "one draw and one copy; no triangle, a draw not delivered, an sRGB image (F1) and a size over the "
                      "target give false with nothing changed (P3-01i); R8 and RG8 images scale down as a draw (P3-08b S2)");
        runMetalTest(136);
    }

    template<> template<>
    void object::test<136>()
    {
        set_test_name("t138: bind(target, true) on a Metal target with a depth binds the device's depth texture; a depth "
                      "sharer stops in the Metal-only check (P3-01f)");
        runMetalTest(138);
    }

    template<> template<>
    void object::test<137>()
    {
        set_test_name("t130: a device's surface is null without a window, with no WARN line, and made on a "
                      "CAMetalLayer as decision 7 says (P3-01f)");
        runMetalTest(130);
    }

    template<> template<>
    void object::test<138>()
    {
        set_test_name("t132: the flip pass draws back buffer row 31 - r into drawable row r, exact (P3-01f)");
        runMetalTest(132);
    }

    template<> template<>
    void object::test<139>()
    {
        set_test_name("t133: resize and 0x0 at the frame boundary; one drawable wait and signal per drawable, also for a "
                      "readback's submit between present and endFrame (P3-01f)");
        runMetalTest(133);
    }

    template<> template<>
    void object::test<140>()
    {
        set_test_name("t140: Capabilities on Metal equals P0-05's list field by field; maxSamples capped at GL's after the "
                      "CGL query, else at 4 (P3-01f)");
        runMetalTest(140);
    }

    template<> template<>
    void object::test<141>()
    {
        set_test_name("t137: a target without alpha in GL with a DontCare load and an RGB-masked draw reads alpha 255: "
                      "the DontCare is a clear to (0, 0, 0, 1) (P3-01f)");
        runMetalTest(137);
    }

    template<> template<>
    void object::test<142>()
    {
        set_test_name("t141: the scissor reaches the native encoder at the draw or clear draw that uses it, only when it "
                      "changed; a new encoder begins at the whole attachment (P3-01f S10b)");
        runMetalTest(141);
    }

    template<> template<>
    void object::test<143>()
    {
        set_test_name("t135: the CGL store's round trip gives back every field the CGL query sets and repeats initGL's "
                      "effects outside LLGLManager; an incomplete store changes nothing (P3-01f S12)");
        runMetalTest(135);
    }

    template<> template<>
    void object::test<144>()
    {
        set_test_name("t139: the CGL store's key differs with the format, the viewer build, the macOS version and build, the "
                      "Mac's memory and the Metal device's name, and is this Mac's (P3-01f S12, review delta-2, delta-3)");
        runMetalTest(139);
    }

    template<> template<>
    void object::test<145>()
    {
        set_test_name("t142: on Metal getGLString gives the CGL query's GL_VENDOR, GL_RENDERER and GL_VERSION and no GL "
                      "call; another name an empty string and one WARN per name (P3-01f, review render-2)");
        runMetalTest(142);
    }

    template<> template<>
    void object::test<146>()
    {
        set_test_name("t143: doMediaTexUpdate's defer_copy on the viewer's Metal device: the parked texture without a GL "
                      "name takes the region and is swapped in by syncTexName(0); the old one goes; no texture leaks "
                      "(P3-01g)");
        runMetalTest(143);
    }

    template<> template<>
    void object::test<147>()
    {
        set_test_name("t145: the Metal encoder counts pass breaks: an upload in a pass closes the segment and is an upload "
                      "break and an upload pass break; an upload with nothing to close is only an upload break; a readback "
                      "in a pass is only a pass break; nothing outside a pass (P3-01g)");
        runMetalTest(145);
    }

    template<> template<>
    void object::test<148>()
    {
        set_test_name("t146: an MSAA 4x pass and its resolve through copyTexture under validation, and the multisampled "
                      "cases not ported (P3-01g, U5)");
        runMetalTest(146);
    }

    template<> template<>
    void object::test<149>()
    {
        set_test_name("t147: the first upload or copy into part of a level and slice nothing has written zeroes the rest, "
                      "as GL's storage without data reads; alpha 1 where GL keeps none; a written level is not zeroed "
                      "again (P3-01g)");
        runMetalTest(147);
    }

    template<> template<>
    void object::test<150>()
    {
        set_test_name("t150: LLRenderTarget::setDepthAccess on Metal: the read-only bind loads the filled depth (a draw "
                      "passes against it only) and refuses a writing draw, the change ends the segment with colour and "
                      "depth stored, the next loads both, and the write lands (P2-06a, F5; P2-R2, R32)");
        runMetalTest(150);
    }

    template<> template<>
    void object::test<151>()
    {
        set_test_name("t148: a dynamic uniform buffer of 49,248 bytes read through a named block and updated whole before "
                      "each of 8 draws in one pass: each draw reads its own bytes, no pass break, no copy, the pool grows "
                      "by one chunk at most (P2-16b)");
        runMetalTest(148);
    }

    template<> template<>
    void object::test<152>()
    {
        set_test_name("t151: createEmptyOwnTexture on Metal makes the image's own texture without a GL name and draws none, "
                      "specifyOwnLevel's levels 0 and 1 read back exactly, and createTextureNames' 10 levels at 512x512 "
                      "read back exactly through specifyTexture (P2-07b, F1' and F1a)");
        runMetalTest(151);
    }

    template<> template<>
    void object::test<153>()
    {
        set_test_name("t152: copyFromBackBuffer on Metal with the back buffer's pass open after a clear: tracked, the pass "
                      "split counted, the GL_RGB target (64, 128, 191, 255), and the segment after the copy loads "
                      "(P2-27, F2)");
        runMetalTest(152);
    }

    template<> template<>
    void object::test<154>()
    {
        set_test_name("t153: copyFromBackBuffer from a 64x48 back buffer into a 48x32 GL_RGB target under API Validation: "
                      "the region probe's behaviour locked (P2-27, F2, C4)");
        runMetalTest(153);
    }

    template<> template<>
    void object::test<155>()
    {
        set_test_name("t155: copyFromBackBuffer from a 48x32 back buffer into a 64x48 GL_RGB target under API Validation: "
                      "tracked, not outside, the overlap's texels exactly and the target's fill around it (P2-27, F2)");
        runMetalTest(155);
    }

    template<> template<>
    void object::test<156>()
    {
        set_test_name("t156: createFloatImage, load_exr's HDRI image, on Metal at 3x2: the image's own 3x2 RGBA16Float "
                      "texture with two levels, no GL name drawn, level 0 holds the floats as halves, no WARN (P2-R2, R18)");
        runMetalTest(156);
    }

    template<> template<>
    void object::test<157>()
    {
        set_test_name("t157: lazy native state (P3-02 D1, F50): after a draw, cull, viewport, depth state, fill mode and "
                      "depth bias each set to two values and one draw: one more native draw with the last values, no "
                      "validation report");
        runMetalTest(157);
    }

    template<> template<>
    void object::test<158>()
    {
        set_test_name("t158: fill mode Lines, depth bias with GL's enables, a clear draw neutral under both, and no bias "
                      "on a line draw (P3-02 D1)");
        runMetalTest(158);
    }

    template<> template<>
    void object::test<159>()
    {
        set_test_name("t159: an integer texture with a linear filter gets Point and mag Nearest, made once (P3-02 D3)");
        runMetalTest(159);
    }

    template<> template<>
    void object::test<160>()
    {
        set_test_name("t160: five present() calls before one endFrame on a CAMetalLayer without a view: each submitted at "
                      "once, five drawables presented with five waits and signals, no 1 s timeout (P3-01h, render-1)");
        runMetalTest(160);
    }

    template<> template<>
    void object::test<161>()
    {
        set_test_name("t161: RGB of U8 from an RGBA8 and an RGBA8_sRGB texture packed to tight RGB rows, 3 bytes a pixel, "
                      "also through LLGPUReadback::copyRows (P3-05a1, F10)");
        runMetalTest(161);
    }

    template<> template<>
    void object::test<162>()
    {
        set_test_name("t162: a channel keeps a slot per texture type: a sampler2D after a cube array on its channel reads "
                      "the 2D slot's white under validation, a null tell empties the cube array's slot (P3-05a1, F11)");
        runMetalTest(162);
    }

    template<> template<>
    void object::test<163>()
    {
        set_test_name("t163: a unit's file and classes from the description name the translator's classes, and the same "
                      "sources at a lower shader level give another disk cache key (P3-04a S2, R1-14)");
        runMetalTest(163);
    }

    template<> template<>
    void object::test<164>()
    {
        set_test_name("t164: the programs of METAL_SAFE_MATH_PROGRAMS and their skinned variants compile with Safe math "
                      "and no other program does; a listed program is Ready (P3-04a S4, decision 8)");
        runMetalTest(164);
    }

    template<> template<>
    void object::test<165>()
    {
        set_test_name("t165: GPU faults: an NSError in a commit feedback is logged with frame, command buffer, domain and "
                      "code, counted without a drain, and loses the device for AccessRevoked and NotPermitted and for a "
                      "Timeout in two command buffers in a row; a frame behind a gate makes the device lost after two "
                      "waits (P3-01i S2, its t163 before the batch merge)");
        runMetalTest(165);
    }

    template<> template<>
    void object::test<170>()
    {
        set_test_name("t170: an upload on the upload thread behind a gate: the main queue's readback after waitForFence "
                      "waits on the GPU and reads the thread's bytes; a texture made and filled on the thread reads as "
                      "the main thread's (P3-03a, C47)");
        runMetalTest(170);
    }

    template<> template<>
    void object::test<171>()
    {
        set_test_name("t171: waitForFence on a null fence, another backend's fence (not called into, one WARN) and the "
                      "recording thread's fence: no queue wait (P3-03a, MT-3)");
        runMetalTest(171);
    }

    template<> template<>
    void object::test<172>()
    {
        set_test_name("t172: an Encoder call from another thread stops in LL_ERRS under the debug setting before the "
                      "tracker changes; without the setting nothing is checked (P3-03a, decision 3)");
        runMetalTest(172);
    }

    template<> template<>
    void object::test<173>()
    {
        set_test_name("t173: the upload thread's empty submit is value 0 and makes no queue; a second thread and "
                      "updateBuffer on the upload thread are refused with one WARN line each (P3-03a)");
        runMetalTest(173);
    }

    template<> template<>
    void object::test<174>()
    {
        set_test_name("t174: generateMipmaps on the upload thread is drawn once at waitForFence on the main encoder and "
                      "equals the main thread's mips: R8 and RGBA8 at 1x8 and 128x128 (P3-03a, choice 5)");
        runMetalTest(174);
    }

    template<> template<>
    void object::test<175>()
    {
        set_test_name("t175: a fetched texture made by createGLTexture on the upload thread is swapped in by the main "
                      "queue's item after the encoder's waitForFence, with no Fence::wait() on the main thread; level 0 "
                      "and the mips read as the main thread's (P3-03a S3)");
        runMetalTest(175);
    }

    template<> template<>
    void object::test<176>()
    {
        set_test_name("t176: doMediaTexUpdate on the media update thread (defer_copy, setSubImage with the name 0, "
                      "syncToMainThread): one deferred thread swap, the new texture reads the frame (P3-03a S3)");
        runMetalTest(176);
    }

    template<> template<>
    void object::test<177>()
    {
        set_test_name("t177: a resize as updateMediaImage makes it gives a new texture, and the update after it is made in "
                      "place in it; a parked texture blocks the in-place update, which swaps a new one in (P3-03c D1)");
        runMetalTest(177);
    }

    template<> template<>
    void object::test<178>()
    {
        set_test_name("t178: the upload converter's fast paths into the staging are byte for byte the general loop's; "
                      "metalUploadSize and a short dst (P3-03c D2)");
        runMetalTest(178);
    }

    template<> template<>
    void object::test<179>()
    {
        set_test_name("t179: large staging kept per ring slot: steady 4 MiB uploads make no buffer, best fit, above "
                      "KEEP_MAX a buffer of its own, trimmed after TRIM_REWINDS idle rewinds (P3-03c D3)");
        runMetalTest(179);
    }

    template<> template<>
    void object::test<180>()
    {
        set_test_name("t180: sample queries count a visible draw's samples exactly, 0 for a hidden draw and a clear, and are "
                      "ready only after their commit ran; result(true) in the frame sends and waits (P3-08)");
        runMetalTest(180);
    }

    template<> template<>
    void object::test<181>()
    {
        set_test_name("t181: a query spans a pass break, two render passes and a begin outside a pass; the first query "
                      "splits an encoder opened without the buffer once (P3-08)");
        runMetalTest(181);
    }

    template<> template<>
    void object::test<182>()
    {
        set_test_name("t182: refused, never begun, abandoned and failed queries read as visible; primitive queries are "
                      "not made (P3-08)");
        runMetalTest(182);
    }

    template<> template<>
    void object::test<183>()
    {
        set_test_name("t183: the visibility ring: a slot zeroed while a gated frame accumulates, a late read after reuse, "
                      "LL_ERRS at the full ring, dropped queries hold no slot (P3-08)");
        runMetalTest(183);
    }

    template<> template<>
    void object::test<184>()
    {
        set_test_name("t184: a ring slot keeps at most KEEP_SLOT_MAX of large staging: a burst's others get a buffer of "
                      "their own, the cap reuses, the trim gives the bytes back (P3-03c's review m1)");
        runMetalTest(184);
    }

    template<> template<>
    void object::test<185>()
    {
        set_test_name("t185: with VSync off a present leaves the frame in the back buffer while the window server "
                      "holds all drawables but one, never for a drawable readback or VSync on, not once stale "
                      "(P3-09 S1, F51)");
        runMetalTest(185);
    }

    template<> template<>
    void object::test<186>()
    {
        set_test_name("t186: the waits by kind with the longest, the stall threshold, the groups' latency, QoS and hash, "
                      "and the MetalCompileStall text (P3-04c B)");
        runMetalTest(186);
    }

    template<> template<>
    void object::test<187>()
    {
        set_test_name("t187: a const array of the GLSL read with an index not known when it compiles, in a Safe math "
                      "program and another, reads its elements without a shader validation report (P3-01k)");
        runMetalTest(187);
    }

    template<> template<>
    void object::test<188>()
    {
        set_test_name("t188: \"Deferred PBR Alpha Shader\" with SSR from its own sources has program-scope constant data, "
                      "so its blended pipeline is a full state, and its draw reads the table without a shader "
                      "validation report (P3-01k)");
        runMetalTest(188);
    }

    template<> template<>
    void object::test<189>()
    {
        set_test_name("t189: one wait translates the whole paused queue and sends every closed group at once, the pool at "
                      "user-initiated QoS; LL_METAL_COMPILE_SLOW gives the behaviour before (P3-04c A, A2)");
        runMetalTest(189);
    }

    template<> template<>
    void object::test<190>()
    {
        set_test_name("t190: a buffer's readback is an ordered GPU copy: static updates before it and not after, an offset, "
                      "a dynamic buffer, in a pass after a draw (a split), a range outside refused (P3-08b S1)");
        runMetalTest(190);
    }

    template<> template<>
    void object::test<191>()
    {
        set_test_name("t191: copies between plain colour formats as a draw give GL's bytes (the shared copy cases: R8, RG8, "
                      "RGB8, A8, LA, an offset region, a first write, a level and a layer, a swizzled source; RGBA16F "
                      "within 1); sRGB, BC, depth and integer refused before the tracker; an MSAA upload refused and not "
                      "counted (P3-08b S2)");
        runMetalTest(191);
    }

    template<> template<>
    void object::test<192>()
    {
        set_test_name("t192: the facades' delivered branches on the viewer's Metal device: rawSnapshot's tile through "
                      "readFramebufferRows (GL's row order, one submit a tile, depth rows of F32, rowPitch width x 4), "
                      "saveChart's and renderMorphMasks' reads, alpha and luminance readbacks false and untracked, "
                      "readBackRaw, setSubImageFromFrameBuffer (P3-08b S3)");
        runMetalTest(192);
    }

    template<> template<>
    void object::test<195>()
    {
        set_test_name("t195: the timestamp heap is made with the device and timerQueries is true; a query from before the "
                      "first draw to after the last costs one split and reads > 0 ns (P3-08 lane B)");
        runMetalTest(195);
    }

    template<> template<>
    void object::test<196>()
    {
        set_test_name("t196: a time query mid-pass splits at its begin and end; spans over a pass break and two passes read "
                      "> 0 ns; the granularity probe; a span around a clear-only pass holds its encoder (P3-08 lane B)");
        runMetalTest(196);
    }

    template<> template<>
    void object::test<197>()
    {
        set_test_name("t197: a time query beside a sample query; refused, never begun, abandoned and failed ones read 0 ns "
                      "and count as timer_invalid; result(true) sends and waits (P3-08 lane B)");
        runMetalTest(197);
    }

    template<> template<>
    void object::test<198>()
    {
        set_test_name("t198: the timer ring: full ring 0 ns with one WARN line, a reused pair keeps its ticks, a signalled "
                      "pair is free within the frame, also for one query begun again (P3-08 lane B)");
        runMetalTest(198);
    }

    template<> template<>
    void object::test<200>()
    {
        set_test_name("t200: an inline test fault on Metal fails the creation at once, before the translator: no WARN, not "
                      "a failed program, one faulted, no group (P3-04b C1)");
        runMetalTest(200);
    }

    template<> template<>
    void object::test<201>()
    {
        set_test_name("t201: a deferred test fault on Metal answers Pending once, then Failed; reflection() takes no "
                      "Pending answer and wait() ends it; each answer is counted (P3-04b C1, F11)");
        runMetalTest(201);
    }

    template<> template<>
    void object::test<202>()
    {
        set_test_name("t202: a test fault of times 2 on Metal fails two creations, the third is translated and Ready, and "
                      "the count starts again (P3-04b C1)");
        runMetalTest(202);
    }

    template<> template<>
    void object::test<203>()
    {
        set_test_name("t203: the test fault takes its exact name only: other names and the clear program are not taken, and "
                      "the faulted program's group-mates are sent and Ready (P3-04b C2)");
        runMetalTest(203);
    }

    template<> template<>
    void object::test<204>()
    {
        set_test_name("t204: getPipeline gives null for an inline or deferred faulted program with one WARN each; the "
                      "programs go cleanly and the device goes on (P3-04b C1)");
        runMetalTest(204);
    }

    template<> template<>
    void object::test<205>()
    {
        set_test_name("t205: Metal's fault list: two names with their own counts and hits, the shared fault wins a name in "
                      "both, arming resets, empty is off (P3-04b F9)");
        runMetalTest(205);
    }

    template<> template<>
    void object::test<206>()
    {
        set_test_name("t206: LLGLSLShader on the viewer's Metal device under a test fault: inline times 1 falls one class "
                      "with its class lines, deferred publishes late and falls, times 0 is refused by bind once (P3-04b)");
        runMetalTest(206);
    }

    template<> template<>
    void object::test<210>()
    {
        set_test_name("t210: the speed knobs from LL_METAL_BARRIER and LL_METAL_DISCARD, the GPU time's overlap rule, its "
                      "window line only past the window and its total and counters when a device goes, the static index "
                      "copies' bytes, all cheap (P3-09b)");
        runMetalTest(210);
    }

    template<> template<>
    void object::test<211>()
    {
        set_test_name("t211: split and full barriers: a copy after a heavy pass reaches the next pass's vertex stage, a "
                      "heavy pass's target reaches a sampling vertex stage (one geometry barrier) and a fragment stage, "
                      "one barrier per native encoder (P3-09b M01)");
        runMetalTest(211);
    }

    template<> template<>
    void object::test<212>()
    {
        set_test_name("t212: with and without discarded stores: a full clear after a draw leaves its segment unstored, a "
                      "colour-only or scissored clear keeps the stores, readbacks before and after the clear are right "
                      "(P3-09b M02)");
        runMetalTest(212);
    }

    template<> template<>
    void object::test<213>()
    {
        set_test_name("t213: a released program's full states leave the device and stay counted; the group hash is the "
                      "records' sum (P3-R4 B-4)");
        runMetalTest(213);
    }

    template<> template<>
    void object::test<214>()
    {
        set_test_name("t214: GPU error lines are written on the main thread, the first ten, and the rest summed up at "
                      "the device's end (P3-R4 B-1)");
        runMetalTest(214);
    }

    template<> template<>
    void object::test<215>()
    {
        set_test_name("t215: a backend's own render encoder after a submit with a debug group open: the command buffer "
                      "first, so it holds no group while the encoder is open");
        runMetalTest(215);
    }

    template<> template<>
    void object::test<216>()
    {
        set_test_name("t216: a flip pass whose binding fails opens no render encoder on the drawable");
        runMetalTest(216);
    }

    template<> template<>
    void object::test<217>()
    {
        set_test_name("t217: a clear that writes nothing does not break the pass");
        runMetalTest(217);
    }

    template<> template<>
    void object::test<218>()
    {
        set_test_name("t218: a native library compiled from source, a compute pipeline and a dispatch into a storage texture; the dispatch marks its level written, so a partial upload keeps the dispatch's texels around it");
        runMetalTest(218);
    }

    template<> template<>
    void object::test<219>()
    {
        set_test_name("t219: a dispatch that reads what the one before it wrote gets a barrier between them, an unrelated one none");
        runMetalTest(219);
    }

    template<> template<>
    void object::test<220>()
    {
        set_test_name("t220: a compute pass inside a stack pass stores the stack pass's work and the stack pass goes on loading, with its pipeline and dynamic state");
        runMetalTest(220);
    }

    template<> template<>
    void object::test<221>()
    {
        set_test_name("t221: a native render pipeline draws a fullscreen triangle of its own vertices in a native pass inside a stack pass, through the native branch of the binding site");
        runMetalTest(221);
    }

    template<> template<>
    void object::test<222>()
    {
        set_test_name("t222: residency follows a level view to its texture");
        runMetalTest(222);
    }

    template<> template<>
    void object::test<223>()
    {
        set_test_name("t223: a 3D texture uploaded slice by slice and read by a kernel");
        runMetalTest(223);
    }

    template<> template<>
    void object::test<224>()
    {
        set_test_name("t224: LLNativeProgram's source with includes and defines from a manifest, compiled by the device");
        runMetalTest(224);
    }

    template<> template<>
    void object::test<225>()
    {
        set_test_name("t225: the common SPD kernel makes every level of a texture in one dispatch");
        runMetalTest(225);
    }

    template<> template<>
    void object::test<226>()
    {
        set_test_name("t226: a translated sampler3D reads the channel's 3D slot, the default 3D texture when it is empty");
        runMetalTest(226);
    }

    template<> template<>
    void object::test<227>()
    {
        set_test_name("t227: without the Metal debug setting an unchanged sampler place takes its remembered texture and sampler, and each change of the channel shows; with it every place is compared and none differs");
        runMetalTest(227);
    }

    template<> template<>
    void object::test<228>()
    {
        set_test_name("t228: a strip's list of an index buffer's indices is kept while the buffer has no update; another range and an update each make their own, static and dynamic, with and without the Metal debug setting");
        runMetalTest(228);
    }

    template<> template<>
    void object::test<229>()
    {
        set_test_name("t229: MetalFX's spatial upscaler keeps a flat colour flat at 2x, 4x and between, scales only the content, "
                      "breaks a stack pass that goes on, and leaves nothing behind when made and let go at changing sizes");
        runMetalTest(229);
    }

    template<> template<>
    void object::test<230>()
    {
        set_test_name("t230: a pass that only clears a depth nothing samples makes no render encoder; the next pass with that "
                      "depth clears it as its load action, and a copy of it or the frame's end encodes the clear first");
        runMetalTest(230);
    }

    template<> template<>
    void object::test<231>()
    {
        set_test_name("t231: with VSync off the window server gets the newest finished frame through the present queue: "
                      "a present flips into a finished frame, endFrame copies it into a drawable once the GPU has "
                      "finished it, held frames wait and only the newest goes, a readback and VSync on present directly");
        runMetalTest(231);
    }
}
