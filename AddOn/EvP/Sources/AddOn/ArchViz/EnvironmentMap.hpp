#ifndef EVP_ARCHVIZ_ENVIRONMENTMAP_HPP
#define EVP_ARCHVIZ_ENVIRONMENTMAP_HPP

// ArchViz/EnvironmentMap — the GPU half of the HDR sky.
//
// It owns ONLY the resources: one equirectangular texture, its view, and the
// nine spherical-harmonic coefficients the CPU side produced. The arithmetic
// lives in ArchViz/EnvironmentLighting, which is Diligent-free and tested
// offline; this file is the part that cannot be tested without a device, and it
// is kept as thin as that split allows -- the same division DiligentShadowMap
// makes against SunShadowMath.
//
// ⚠️ THE TEXTURE IS ALLOCATED ONCE AT A FIXED SIZE AND ONLY ITS CONTENTS
// CHANGE. This is not a simplification, it is a BINDING CONSTRAINT, and getting
// it wrong would fail in a way that is hard to read. The scene binds its shader
// resources as STATIC variables at pipeline-creation time (DiligentScene.cpp),
// which the shadow map gets away with for exactly this reason -- it is created
// once and refilled. A texture whose dimensions followed whatever HDR the user
// loaded would have to be RECREATED per load, and a recreated texture is a
// different object that the already-built SRBs still do not point at: the sky
// would load successfully, report success, and never appear. So every HDR is
// resampled to kWidth x kHeight on the CPU before upload.
//
// ⚠️ RENDER THREAD ONLY, like everything in the Diligent viewport. The load
// reads a file, which is slow; it is called from the frame loop when a path
// arrives over the bus, NOT from the bus thread, because the device context is
// not thread-safe.
//
// ⚠️ NO ACAPI. The path arrives as a string from the Python side (the file
// dialog problem: HANDOFF-RenderingPanels caveat #4 -- an ImGui file dialog
// blocks the render thread inside Archicad's process).

#include <cstdint>
#include <string>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
struct ITextureView;
}   // namespace Diligent

namespace geomsrv {
namespace archviz {

class EnvironmentMap final {
public:
    // ⚠️ 2048x1024 BECAUSE THE SKY IS NOW *LOOKED AT*, NOT ONLY SAMPLED. The
    // old 512x256 was sized for lighting alone, and the reasoning was sound for
    // that: the diffuse comes entirely from the SH (resolution irrelevant) and
    // the specular is a roughness-blurred lookup. Drawing the environment as
    // the BACKGROUND broke the premise -- it is read at screen resolution, and
    // a 45-degree window onto a 512-wide panorama is 64 texels stretched across
    // 1490 pixels. A 23x magnification, reported live as "the HDR scale feels
    // like 1000 times bigger". It is not scale; it is resolution.
    //
    // 2048 is the width of an ordinary 2k HDR, so the common case is now
    // resampled 1:1 instead of thrown away. About 21 MB with the mip chain at
    // RGBA16F. Raising these is the documented way to grow the sky; making the
    // size FOLLOW THE FILE is still forbidden -- see the note below on why the
    // texture is allocated once.
    static constexpr uint32_t kWidth = 2048;
    static constexpr uint32_t kHeight = 1024;

    EnvironmentMap ();
    ~EnvironmentMap ();
    EnvironmentMap (const EnvironmentMap&) = delete;
    EnvironmentMap& operator= (const EnvironmentMap&) = delete;

    // Allocates the textures and builds the prefilter pipeline. Does NOT load
    // anything -- until Load succeeds the map is Ready() but not Loaded(), and
    // the shader's own gate keeps it unread.
    //
    // ⚠️ TWO TEXTURES, NOT ONE, SINCE RE51.B6. The upload lands in a private
    // SOURCE texture with an ordinary box mip chain, and the prefilter reads
    // that while writing the PUBLIC one whose view `ShaderView` returns. They
    // cannot be the same object: a D3D11 draw that reads and writes one texture
    // has its SRV silently unbound, which renders a black sky and says nothing.
    // Both are allocated once here, for the binding reason above.
    bool Init (Diligent::IRenderDevice* device, std::string& error);
    void Shutdown ();
    bool IsReady () const;

    // Read an .hdr from disk, resample, upload, generate mips, and project the
    // SH. Returns false and fills `error` without disturbing the previously
    // loaded sky -- a failed load must not blank a working environment.
    bool Load (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
               const char* path, std::string& error);

    // Forget the sky. The texture stays allocated (see the ⚠️ above); only the
    // loaded flag and the coefficients are cleared.
    void Clear ();

    bool IsLoaded () const;
    const char* LoadedPath () const;

    // The equirect view to bind as `g_envMap`. Never null once Init succeeded,
    // so the binding can be made once at pipeline creation.
    Diligent::ITextureView* ShaderView () const;

    // The nine RGB coefficients, laid out for the constant buffer as 9 float4s
    // (xyz used, w ignored). Zero until a successful Load.
    void CopyShCoefficients (float out[9][4]) const;

    // How many mip levels the texture has -- the shader needs it to turn a
    // roughness into a mip index.
    uint32_t MipLevels () const;

    // ---- RE51.B6, the GGX prefilter -----------------------------------------

    // Whether the last successful Load ran the prefilter to completion.
    //
    // ⚠️ FALSE IS A WORKING RENDERER, NOT A BROKEN ONE, and that is why it is a
    // separate flag from IsLoaded. If the prefilter pipeline could not be built
    // -- an old driver, a shader that failed to compile -- the map falls back to
    // the BOX mip chain it had before this unit, which is the behaviour every
    // image before 2026-08-21 was rendered with. The sky still loads, the model
    // still reflects it, and only a mirror looks worse than it should. A
    // prefilter that failed loudly and blanked the sky would be a far worse
    // trade.
    bool IsPrefiltered () const;

    // How many mips the last Load actually re-rendered (0 when it fell back),
    // and how long that took. ⚠️ REPORTED BECAUSE IT IS THE ONLY EVIDENCE. A
    // prefiltered mip and a box-filtered one are both "a blurrier sky"; nothing
    // on screen distinguishes a prefilter that ran from one that silently did
    // not, so the probe reads these two numbers instead of judging pixels.
    uint32_t PrefilteredMips () const;
    double PrefilterMilliseconds () const;

    // Why the prefilter did not run, empty when it did. Never blocks a load.
    const char* PrefilterError () const;

    // The solid-angle-weighted mean radiance of the loaded sky.
    //
    // ⚠️ IT IS REPORTED BECAUSE A BLACK SKY AND A FAILED BINDING LOOK THE SAME.
    // An HDR that loads, resamples and uploads perfectly but happens to be all
    // zeros renders exactly like a texture that was never bound, and no debug
    // view distinguishes them. One number does.
    void AverageRadiance (float out[3]) const;

    // ⚠️ PUBLIC AS A NAME ONLY, AND STILL OPAQUE. The definition lives in the
    // .cpp and nothing outside it can do anything with this. It is spelled
    // public so the prefilter helpers there -- which are file-local free
    // functions, not members, because they are Diligent plumbing rather than
    // part of this class's contract -- may NAME the type in their signatures.
    struct Impl;

private:
    Impl* impl_;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
