// Compiles Luminous's hand-written MSL offline with the system's runtime compiler: every library of
// every programs.json, with its includes resolved as the viewer resolves them (llmslinclude.h), the
// same compile options as the Metal backend, and each compute and render pipeline the manifest lists.
// Every *shared.h under shaders/metal/luminous is also compiled as an MSL unit of its own, so the
// layout checks in those headers hold on the MSL side too.
//
// No GPU work: a device and an MTL4Compiler are made, never a queue, a command buffer or a dispatch.
// Built and run by luminous/scripts/msl-check.
//
// Usage: msl-check-bin ROOT [programs.json ...]
//   ROOT is indra/newview/app_settings/shaders/metal; without manifests every programs.json under it.
// Prints OK or FAIL per item, with the compiler's messages; exit 1 when anything failed.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "llmslinclude.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int gFailures = 0;
    int gChecks = 0;

    void report(bool ok, const std::string& what, NSError* error)
    {
        ++gChecks;
        if (!ok)
        {
            ++gFailures;
        }
        std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
        if (error)
        {
            std::printf("%s\n", error.localizedDescription.UTF8String);
        }
    }

    void fail(const std::string& what, const std::string& why)
    {
        ++gChecks;
        ++gFailures;
        std::printf("FAIL %s\n%s\n", what.c_str(), why.c_str());
    }

    std::optional<std::string> readFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::nullopt;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    /// The Metal backend's options (metalCompileOptions): MSL 4.0, relaxed math with fast functions.
    MTLCompileOptions* compileOptions(NSDictionary* defines)
    {
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion4_0;
        options.mathMode = MTLMathModeRelaxed;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast;
        if (defines.count)
        {
            options.preprocessorMacros = defines;
        }
        return options;
    }

    /// llrhi.h's Format names, as programs.json writes them.
    MTLPixelFormat pixelFormat(NSString* name)
    {
        static NSDictionary<NSString*, NSNumber*>* formats = @{
            @"R8Unorm" : @(MTLPixelFormatR8Unorm),
            @"RG8Unorm" : @(MTLPixelFormatRG8Unorm),
            @"RGBA8Unorm" : @(MTLPixelFormatRGBA8Unorm),
            @"RGBA8Unorm_sRGB" : @(MTLPixelFormatRGBA8Unorm_sRGB),
            @"BGRA8Unorm" : @(MTLPixelFormatBGRA8Unorm),
            @"R16Float" : @(MTLPixelFormatR16Float),
            @"RG16Float" : @(MTLPixelFormatRG16Float),
            @"RGBA16Float" : @(MTLPixelFormatRGBA16Float),
            @"RGBA16Unorm" : @(MTLPixelFormatRGBA16Unorm),
            @"R32Float" : @(MTLPixelFormatR32Float),
            @"RG32Float" : @(MTLPixelFormatRG32Float),
            @"RGBA32Float" : @(MTLPixelFormatRGBA32Float),
            @"RGB10A2Unorm" : @(MTLPixelFormatRGB10A2Unorm),
            @"RG11B10Float" : @(MTLPixelFormatRG11B10Float),
            @"R8Uint" : @(MTLPixelFormatR8Uint),
            @"R16Uint" : @(MTLPixelFormatR16Uint),
            @"R32Uint" : @(MTLPixelFormatR32Uint),
            @"Depth32Float" : @(MTLPixelFormatDepth32Float),
            @"Depth32Float_Stencil8" : @(MTLPixelFormatDepth32Float_Stencil8),
        };
        NSNumber* format = name ? formats[name] : nil;
        return format ? MTLPixelFormat(format.unsignedIntegerValue) : MTLPixelFormatInvalid;
    }

    /// Sets one attachment's blending from the manifest's name; false for an unknown name.
    bool setBlend(MTL4RenderPipelineColorAttachmentDescriptor* attachment, NSString* blend)
    {
        if (!blend || [blend isEqualToString:@"none"])
        {
            attachment.blendingState = MTL4BlendStateDisabled;
            return true;
        }
        attachment.blendingState = MTL4BlendStateEnabled;
        attachment.rgbBlendOperation = MTLBlendOperationAdd;
        attachment.alphaBlendOperation = MTLBlendOperationAdd;
        if ([blend isEqualToString:@"additive"])
        {
            attachment.sourceRGBBlendFactor = attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
            attachment.destinationRGBBlendFactor = attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
        }
        else if ([blend isEqualToString:@"alpha"])
        {
            attachment.sourceRGBBlendFactor = attachment.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            attachment.destinationRGBBlendFactor = attachment.destinationAlphaBlendFactor =
                MTLBlendFactorOneMinusSourceAlpha;
        }
        else if ([blend isEqualToString:@"multiply"])
        {
            attachment.sourceRGBBlendFactor = attachment.sourceAlphaBlendFactor = MTLBlendFactorDestinationColor;
            attachment.destinationRGBBlendFactor = attachment.destinationAlphaBlendFactor = MTLBlendFactorZero;
        }
        else
        {
            return false;
        }
        return true;
    }

    id<MTLLibrary> compile(id<MTL4Compiler> compiler, const std::string& name, const std::string& source,
                           NSDictionary* defines)
    {
        MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
        descriptor.name = @(name.c_str());
        descriptor.source = @(source.c_str());
        descriptor.options = compileOptions(defines);
        NSError* error = nil;
        id<MTLLibrary> library = [compiler newLibraryWithDescriptor:descriptor error:&error];
        report(library != nil, "library " + name, library ? nil : error);
        return library;
    }

    /// Whether the library has the function; Metal stops the process on a pipeline whose function is missing.
    bool hasFunction(id<MTLLibrary> library, NSString* name)
    {
        return name && [library.functionNames containsObject:name];
    }

    MTL4LibraryFunctionDescriptor* function(id<MTLLibrary> library, NSString* name)
    {
        MTL4LibraryFunctionDescriptor* descriptor = [MTL4LibraryFunctionDescriptor new];
        descriptor.library = library;
        descriptor.name = name;
        return descriptor;
    }

    void checkManifest(id<MTL4Compiler> compiler, const fs::path& root, const fs::path& manifest)
    {
        const std::string where = fs::relative(manifest, root).string();
        NSData* data = [NSData dataWithContentsOfFile:@(manifest.c_str())];
        NSError* error = nil;
        NSDictionary* json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        NSArray* libraries = [json isKindOfClass:[NSDictionary class]] ? json[@"libraries"] : nil;
        if (![libraries isKindOfClass:[NSArray class]])
        {
            fail(where, error ? error.localizedDescription.UTF8String : "no \"libraries\" array");
            return;
        }
        const LL::MSL::ReadFile read = [&root](const std::string& path) { return readFile(root / path); };
        for (NSDictionary* entry in libraries)
        {
            const std::string name = [entry[@"name"] UTF8String] ?: "(unnamed)";
            const std::string label = where + " " + name;
            std::string source;
            bool resolved = true;
            for (NSString* file in entry[@"files"])
            {
                const LL::MSL::IncludeResult result = LL::MSL::resolveIncludes(file.UTF8String, read);
                if (!result.error.empty())
                {
                    fail("library " + label, result.error);
                    resolved = false;
                    break;
                }
                source += result.text;
            }
            if (!resolved)
            {
                continue;
            }
            NSMutableDictionary* defines = [NSMutableDictionary new];
            NSDictionary* given = entry[@"defines"];
            for (NSString* key in given)
            {
                defines[key] = [given[key] description];
            }
            id<MTLLibrary> library = compile(compiler, label, source, defines);
            if (!library)
            {
                continue;
            }
            for (NSDictionary* compute in entry[@"compute"])
            {
                NSString* name = compute[@"function"];
                if (!hasFunction(library, name))
                {
                    fail("compute " + label + " " + (name ? name.UTF8String : "(none)"), "no such function");
                    continue;
                }
                MTL4ComputePipelineDescriptor* descriptor = [MTL4ComputePipelineDescriptor new];
                descriptor.computeFunctionDescriptor = function(library, name);
                NSArray* size = compute[@"threadsPerThreadgroup"];
                if (size.count == 3 && [size[0] unsignedIntegerValue] != 0)
                {
                    descriptor.requiredThreadsPerThreadgroup =
                        MTLSizeMake([size[0] unsignedIntegerValue], [size[1] unsignedIntegerValue],
                                    [size[2] unsignedIntegerValue]);
                }
                NSError* failure = nil;
                id<MTLComputePipelineState> state = [compiler newComputePipelineStateWithDescriptor:descriptor
                                                                                compilerTaskOptions:nil
                                                                                              error:&failure];
                report(state != nil, "compute " + label + " " + name.UTF8String, state ? nil : failure);
            }
            for (NSDictionary* render in entry[@"render"])
            {
                NSString* vertex = render[@"vertex"];
                NSString* fragment = render[@"fragment"];
                NSString* blend = render[@"blend"];
                const std::string what = "render " + label + " " + (vertex ? vertex.UTF8String : "(none)") + " " +
                                         (fragment ? fragment.UTF8String : "(none)") +
                                         " (" + (blend ? blend.UTF8String : "none") + ")";
                if (!hasFunction(library, vertex) || !hasFunction(library, fragment))
                {
                    fail(what, "no such function");
                    continue;
                }
                MTL4RenderPipelineDescriptor* descriptor = [MTL4RenderPipelineDescriptor new];
                descriptor.vertexFunctionDescriptor = function(library, vertex);
                descriptor.fragmentFunctionDescriptor = function(library, fragment);
                descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
                NSArray* colors = render[@"colorFormats"];
                bool valid = colors.count <= 8;
                for (NSUInteger i = 0; valid && i < colors.count; ++i)
                {
                    const MTLPixelFormat format = pixelFormat(colors[i]);
                    valid = format != MTLPixelFormatInvalid;
                    descriptor.colorAttachments[i].pixelFormat = format;
                    valid = valid && setBlend(descriptor.colorAttachments[i], blend);
                }
                if (!valid)
                {
                    fail(what, "an unknown color format or blend");
                    continue;
                }
                NSString* depth = render[@"depthFormat"];
                if (depth.length && pixelFormat(depth) == MTLPixelFormatInvalid)
                {
                    fail(what, "an unknown depth format");
                    continue;
                }
                // MTL4RenderPipelineDescriptor has no depth format: the pass decides it, so a depth
                // format only needs to be known.
                NSError* failure = nil;
                id<MTLRenderPipelineState> state = [compiler newRenderPipelineStateWithDescriptor:descriptor
                                                                              compilerTaskOptions:nil
                                                                                            error:&failure];
                report(state != nil, what, state ? nil : failure);
            }
        }
    }

    void checkSharedHeader(id<MTL4Compiler> compiler, const fs::path& root, const fs::path& header)
    {
        const std::string path = fs::relative(header, root).generic_string();
        const LL::MSL::ReadFile read = [&root](const std::string& file) { return readFile(root / file); };
        const LL::MSL::IncludeResult result = LL::MSL::resolveIncludes(path, read);
        if (!result.error.empty())
        {
            fail("shared " + path, result.error);
            return;
        }
        const std::string source = "#include <metal_stdlib>\n" + result.text +
                                   "\nkernel void luminous_shared_header_check() {}\n";
        MTL4LibraryDescriptor* descriptor = [MTL4LibraryDescriptor new];
        descriptor.name = @(path.c_str());
        descriptor.source = @(source.c_str());
        descriptor.options = compileOptions(nil);
        NSError* error = nil;
        id<MTLLibrary> library = [compiler newLibraryWithDescriptor:descriptor error:&error];
        report(library != nil, "shared " + path, library ? nil : error);
    }
}

int main(int argc, const char** argv)
{
    @autoreleasepool
    {
        if (argc < 2)
        {
            std::fprintf(stderr, "usage: %s ROOT [programs.json ...]\n", argv[0]);
            return 2;
        }
        const fs::path root = fs::absolute(argv[1]);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device)
        {
            std::fprintf(stderr, "no Metal device\n");
            return 2;
        }
        NSError* error = nil;
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        if (!compiler)
        {
            std::fprintf(stderr, "no MTL4Compiler: %s\n", error.localizedDescription.UTF8String);
            return 2;
        }

        std::vector<fs::path> manifests;
        std::vector<fs::path> headers;
        if (argc > 2)
        {
            for (int i = 2; i < argc; ++i)
            {
                manifests.push_back(fs::absolute(argv[i]));
            }
        }
        else
        {
            for (const auto& entry : fs::recursive_directory_iterator(root))
            {
                if (entry.path().filename() == "programs.json")
                {
                    manifests.push_back(entry.path());
                }
            }
        }
        const fs::path luminous = root / "luminous";
        if (fs::exists(luminous))
        {
            for (const auto& entry : fs::recursive_directory_iterator(luminous))
            {
                const std::string name = entry.path().filename().string();
                if (name.size() > 8 && name.compare(name.size() - 8, 8, "shared.h") == 0)
                {
                    headers.push_back(entry.path());
                }
            }
        }
        std::sort(manifests.begin(), manifests.end());
        std::sort(headers.begin(), headers.end());
        for (const fs::path& manifest : manifests)
        {
            checkManifest(compiler, root, manifest);
        }
        for (const fs::path& header : headers)
        {
            checkSharedHeader(compiler, root, header);
        }
        std::printf("%d checks, %d failed\n", gChecks, gFailures);
        return gFailures ? 1 : 0;
    }
}
