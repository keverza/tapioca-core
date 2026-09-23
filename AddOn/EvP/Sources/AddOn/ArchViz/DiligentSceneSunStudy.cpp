// The sun study's display path: the atlas as a texture, one side buffer per
// element, and the pass that tints the model with it.
//
// ⚠️ WHY THIS IS ITS OWN TRANSLATION UNIT. DiligentSceneImpl.hpp splits the
// scene by concern and tools/quality/check_cpp.py freezes the sizes of the
// three files that exist; DiligentSceneDraw.cpp is at the 1000-line cap and its
// own note says the next feature EXTRACTS rather than grows. DiligentSceneOcclusion.cpp
// is the precedent: one pass, its one pipeline state, and the lazy build tying
// them together.
//
// ⚠️ THE MAPPING IS NOT HERE. Which atlas tile a rendered triangle reads, and
// through which permutation, is ArchViz/SunStudyOverlay.cpp — pure, and checked
// numerically in tests/cpp/test_sunstudyoverlay.cpp. Everything in this file is
// GPU plumbing, and the reason for that division is that a wrong tile renders as
// a plausible study: it cannot be found by looking at a viewport, so it must not
// live where only a viewport can reach it.

#include "ArchViz/DiligentSceneImpl.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/DiligentShaders.hpp"

#include <Sampler.h>
#include <Texture.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace geomsrv {
namespace archviz {

bool DiligentScene::CreateSunStudyPipeline (Diligent::IRenderDevice* device, uint32_t colorBufferFormat,
                                            uint32_t depthBufferFormat, std::string& error)
{
    if (device == nullptr || impl_ == nullptr || impl_->vs == nullptr) {
        error = "the sun study tint needs the scene's mesh VS, which Init builds first";
        return false;
    }

    if (impl_->sunTintPs == nullptr) {
        const std::string source = ArchVizShaderSource (kArchVizSunTintPS);
        Diligent::ShaderCreateInfo sci;
        sci.Desc.Name = "ArchViz sun study tint PS";
        sci.Desc.ShaderType = Diligent::SHADER_TYPE_PIXEL;
        sci.EntryPoint = "main";
        sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
        sci.Source = source.c_str ();
        sci.SourceLength = source.size ();
        device->CreateShader (sci, &impl_->sunTintPs, nullptr);
        if (impl_->sunTintPs == nullptr) {
            error = "Diligent CreateShader(ArchViz sun study tint PS) failed -- the HLSL compiler's own "
                    "message is in the debug output";
            return false;
        }
    }

    // ⚠️ THE SAME THREE ELEMENTS AS EVERY OTHER MESH PIPELINE. The tint reuses
    // the scene's vertex buffer untouched -- that is the whole point of the side
    // car -- so it reads ArchVizVertex exactly as the shaded pass does.
    const Diligent::LayoutElement layout[] = {
        Diligent::LayoutElement { 0, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // position
        Diligent::LayoutElement { 1, 0, 3, Diligent::VT_FLOAT32, Diligent::False }, // normal
        Diligent::LayoutElement { 2, 0, 4, Diligent::VT_UINT8, Diligent::True },    // colour
    };

    // The atlas is point-sampled and clamped. ⚠️ POINT, NOT BILINEAR, AND THE
    // SENTINEL IS WHY. A bilinear tap near a tile edge mixes the gutter's -1
    // into a real reading, and a negative-going average is neither "no sample"
    // nor the hours that were measured -- it is a value no engine produced,
    // smeared along every face boundary. Filtering can arrive once the sampler
    // dilates its gutters, which the plan already owns.
    Diligent::SamplerDesc atlasSampler;
    atlasSampler.MinFilter = Diligent::FILTER_TYPE_POINT;
    atlasSampler.MagFilter = Diligent::FILTER_TYPE_POINT;
    atlasSampler.MipFilter = Diligent::FILTER_TYPE_POINT;
    atlasSampler.AddressU = Diligent::TEXTURE_ADDRESS_CLAMP;
    atlasSampler.AddressV = Diligent::TEXTURE_ADDRESS_CLAMP;
    atlasSampler.AddressW = Diligent::TEXTURE_ADDRESS_CLAMP;

    const Diligent::ShaderResourceVariableDesc variables[] = {
        // Per element, so it changes between draws inside one pass.
        { Diligent::SHADER_TYPE_PIXEL, "g_sunFaceMaps", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
        // Per study, but the texture is recreated whenever a study is replaced,
        // so it cannot be STATIC either.
        { Diligent::SHADER_TYPE_PIXEL, "g_sunAtlas", Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC },
    };
    // ⚠️ NAMED `g_sunAtlas_sampler`, THE SAMPLER VARIABLE, NOT THE TEXTURE.
    // That is what every other immutable sampler in this renderer is named
    // (`g_shadowMap_sampler`, `g_envMap_sampler`), and a name that matches
    // nothing is not an error -- the shader simply gets a default sampler, which
    // for this pass would be LINEAR and would blend the gutter's sentinel into
    // every tile edge.
    const Diligent::ImmutableSamplerDesc samplers[] = {
        { Diligent::SHADER_TYPE_PIXEL, "g_sunAtlas_sampler", atlasSampler },
    };

    for (int depthIdx = 0; depthIdx < kSunDepthModeCount; ++depthIdx) {
        for (int cullIdx = 0; cullIdx < kCullModeCount; ++cullIdx) {
            const CullMode cull = cullIdx == 0 ? CullMode::Ccw : (cullIdx == 1 ? CullMode::Cw : CullMode::None);

            Diligent::GraphicsPipelineStateCreateInfo pci;
            pci.PSODesc.Name = "ArchViz sun study tint PSO";
            pci.PSODesc.ResourceLayout.DefaultVariableType = Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC;
            pci.PSODesc.ResourceLayout.Variables = variables;
            pci.PSODesc.ResourceLayout.NumVariables = _countof (variables);
            pci.PSODesc.ResourceLayout.ImmutableSamplers = samplers;
            pci.PSODesc.ResourceLayout.NumImmutableSamplers = _countof (samplers);

            Diligent::GraphicsPipelineDesc& gp = pci.GraphicsPipeline;
            gp.NumRenderTargets = 1;
            gp.RTVFormats[0] = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
            gp.DSVFormat = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
            gp.PrimitiveTopology = Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            gp.RasterizerDesc.CullMode = ToDiligentCull (cull);
            gp.RasterizerDesc.FillMode = Diligent::FILL_MODE_SOLID;
            gp.RasterizerDesc.FrontCounterClockwise = Diligent::False;

            // ⚠️ THE DEFAULT MODE TESTS LESS_EQUAL AND WRITES DEPTH, so GLASS IS A
            // SURFACE LIKE ANY OTHER. The study measures every element as one
            // material -- glass receives samples and casts shadow -- so the display
            // must show the FRONTMOST surface's hours, transparent or not.
            //
            // Why that is still a decal: the tint runs through the SAME vertex
            // shader and the SAME view-projection as the shaded pass, so every
            // fragment of a surface that pass wrote is bit-identical to the depth
            // already stored, and passes LESS_EQUAL exactly as it passed EQUAL --
            // no z-fight, no flicker. The only fragments NEARER than the stored
            // depth belong to surfaces the shaded pass did not write, and that is
            // precisely the transparent pass (DiligentScene.cpp: it never writes
            // depth). Plain EQUAL rejected every glass fragment and then painted
            // the wall BEHIND the glass opaquely over it: glass "ignored".
            //
            // The WRITE makes the frontmost fragment win whatever order the
            // elements are drawn in. It is safe here and was not when this was
            // first written: the tint now runs after the transparent pass, and
            // only the line overlays follow it.
            //
            // `LessEqual` keeps its old meaning (no write) as the fallback for a
            // depth buffer that is not bit-identical.
            // ⚠️ ALWAYS DISABLES THE TEST RATHER THAN PASSING IT. A test that always
            // passes still consumes the depth buffer; disabling it is what proves the
            // depth buffer is not involved at all, which is the whole point of the
            // third variant.
            const bool frontmost = depthIdx == int (SunStudyDepthMode::Equal);
            gp.DepthStencilDesc.DepthEnable =
                depthIdx == int (SunStudyDepthMode::Always) ? Diligent::False : Diligent::True;
            gp.DepthStencilDesc.DepthWriteEnable = frontmost ? Diligent::True : Diligent::False;
            gp.DepthStencilDesc.DepthFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;

            // ⚠️ OPAQUE REPLACEMENT, NOT A BLEND. The first tint has one job: to
            // prove that a texel lands on the surface it was measured on. Blending
            // it with the shading underneath makes a mapping error look like a
            // lighting variation, which is the one thing this pass must not do.
            Diligent::RenderTargetBlendDesc& rt = gp.BlendDesc.RenderTargets[0];
            rt.BlendEnable = Diligent::False;

            gp.InputLayout.LayoutElements = layout;
            gp.InputLayout.NumElements = _countof (layout);

            pci.pVS = impl_->vs;
            pci.pPS = impl_->sunTintPs;
            device->CreateGraphicsPipelineState (pci, &impl_->sunTintPso[depthIdx][cullIdx]);
            if (impl_->sunTintPso[depthIdx][cullIdx] == nullptr) {
                error = "Diligent CreateGraphicsPipelineState(ArchViz sun study tint) failed";
                return false;
            }
            for (Diligent::SHADER_TYPE stage : { Diligent::SHADER_TYPE_VERTEX, Diligent::SHADER_TYPE_PIXEL }) {
                if (Diligent::IShaderResourceVariable* cb =
                        impl_->sunTintPso[depthIdx][cullIdx]->GetStaticVariableByName (stage, "ArchVizConstants"))
                    cb->Set (impl_->constants);
            }
            // One SRB per cull mode, made from the FIRST depth variant and reused by
            // the other two -- they declare an identical resource layout.
            if (impl_->sunTintSrb[cullIdx] == nullptr) {
                impl_->sunTintPso[depthIdx][cullIdx]->CreateShaderResourceBinding (&impl_->sunTintSrb[cullIdx], true);
                if (impl_->sunTintSrb[cullIdx] == nullptr) {
                    error = "Diligent CreateShaderResourceBinding(ArchViz sun study tint) failed";
                    return false;
                }
            }
        }
    }
    return true;
}

void DiligentScene::ClearSunStudy ()
{
    if (impl_ == nullptr)
        return;
    impl_->sunStudyPayload.reset ();
    for (Entry& e : impl_->elements)
        e.sunFaceBuffer.Release ();
    impl_->sunAtlasSRV = nullptr;
    impl_->sunAtlasTexture.Release ();
    impl_->sunStudyId.clear ();
    impl_->sunStudyVersion = 0;
    impl_->sunAtlasWidth = 0;
    impl_->sunAtlasHeight = 0;
    impl_->sunHoursMax = 1.0f;
    impl_->sunDebugMode = 0;
    impl_->sunQuantumHours = 0.25f;
    impl_->sunElementsNamed = 0;
    impl_->sunElementsAttached = 0;
    impl_->sunElementsAbsent = 0;
    impl_->sunRefusedTriangleCount = 0;
    impl_->sunRefusedTopologyHash = 0;
    // ⚠️ THE REJECTION REASON IS CLEARED HERE AND THE UPLOAD COUNTERS ARE NOT.
    // A clear is a deliberate act with nothing wrong about it, so it must not
    // leave the last refusal's sentence standing; the lifetime upload counters
    // answer a question about the VIEWPORT and survive on purpose.
    impl_->sunRejection.clear ();
}

void DiligentScene::ApplySunStudy (Diligent::IRenderDevice* device, std::unique_ptr<SunStudyAtlasUpload> study)
{
    if (impl_ == nullptr)
        return;

    // ⚠️ THE PREVIOUS STUDY GOES FIRST, UNCONDITIONALLY. Whatever happens below,
    // the resources that described the old atlas must not survive into a frame
    // that binds the new one: a side buffer holding the OLD packing indexes the
    // NEW texture with tiles that have moved, and every face then reads some
    // other face's hours. Nothing about that looks like an error.
    ClearSunStudy ();

    if (device == nullptr || study == nullptr)
        return;
    if (study->width == 0 || study->height == 0 || study->texels == nullptr) {
        impl_->sunRejection = "the study carried no atlas image";
        ArchVizLog ("Diligent scene: sun study refused -- " + impl_->sunRejection);
        return;
    }
    if (study->texels->size () != static_cast<size_t> (study->width) * study->height) {
        impl_->sunStudyId = study->studyId;
        impl_->sunRejection = "the atlas image is " + std::to_string (study->texels->size ()) + " texels for a " +
                              std::to_string (study->width) + "x" + std::to_string (study->height) + " atlas";
        ArchVizLog ("Diligent scene: sun study '" + study->studyId + "' refused -- " + impl_->sunRejection);
        return;
    }

    Diligent::TextureDesc desc;
    desc.Name = "ArchViz sun study atlas";
    desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
    desc.Width = study->width;
    desc.Height = study->height;
    desc.MipLevels = 1;
    // ⚠️ R32_FLOAT, AND NOT A NORMALISED FORMAT. The texels are HOURS, and the
    // sentinel is NEGATIVE; a UNORM format cannot represent either, and would
    // turn "no sample here" into "nought hours" -- a shadow that is not there,
    // which is the one failure the whole atlas design is written against.
    desc.Format = Diligent::TEX_FORMAT_R32_FLOAT;
    desc.Usage = Diligent::USAGE_IMMUTABLE;
    desc.BindFlags = Diligent::BIND_SHADER_RESOURCE;

    Diligent::TextureSubResData level;
    level.pData = study->texels->data ();
    level.Stride = static_cast<Diligent::Uint64> (study->width) * sizeof (float);
    Diligent::TextureData data;
    data.pSubResources = &level;
    data.NumSubresources = 1;

    device->CreateTexture (desc, &data, &impl_->sunAtlasTexture);
    if (impl_->sunAtlasTexture == nullptr) {
        impl_->sunStudyId = study->studyId;
        impl_->sunRejection = "the " + std::to_string (study->width) + "x" + std::to_string (study->height) +
                              " R32_FLOAT atlas texture could not be created";
        ArchVizLog ("Diligent scene: sun study '" + study->studyId + "' refused -- " + impl_->sunRejection);
        return;
    }
    ++impl_->sunAtlasUploads;
    impl_->sunAtlasBytesUploaded += static_cast<uint64_t> (study->texels->size ()) * sizeof (float);
    impl_->sunAtlasSRV = impl_->sunAtlasTexture->GetDefaultView (Diligent::TEXTURE_VIEW_SHADER_RESOURCE);

    impl_->sunStudyId = study->studyId;
    impl_->sunStudyVersion = study->version;
    impl_->sunAtlasWidth = study->width;
    impl_->sunAtlasHeight = study->height;
    impl_->sunHoursMax = study->hoursMax > 0.0f ? study->hoursMax : 1.0f;
    impl_->sunDebugMode = study->debugMode;
    impl_->sunQuantumHours = study->quantumHours > 0.0f ? study->quantumHours : 0.25f;
    impl_->sunDepthMode = study->depthMode < uint32_t (kSunDepthModeCount) ? study->depthMode : 0;
    // ⚠️ THE PAYLOAD IS RETAINED BEFORE THE BIND, not after: AttachSunStudy reads
    // it, and every later EndBatch reads it again.
    impl_->sunStudyPayload = std::move (study);
    AttachSunStudy (device);
}

void DiligentScene::AttachSunStudy (Diligent::IRenderDevice* device)
{
    if (impl_ == nullptr || device == nullptr || impl_->sunStudyPayload == nullptr)
        return;

    const SunStudyAtlasUpload& study = *impl_->sunStudyPayload;
    const size_t previousAttached = impl_->sunElementsAttached;
    const size_t previousAbsent = impl_->sunElementsAbsent;
    const size_t previousRefusedCount = impl_->sunRefusedTriangleCount;
    const size_t previousRefusedHash = impl_->sunRefusedTopologyHash;
    size_t attached = 0;
    size_t refusedHash = 0;
    size_t refusedCount = 0;
    size_t absent = 0;
    for (const SunStudyElementMap& map : study.elements) {
        Entry* entry = impl_->Find (map.guid);

        // ⚠️ THE RULE IS NOT HERE. ClassifySunFaceBinding is pure and lives in
        // ArchViz/SunStudyOverlay.cpp, where tests/cpp can run every one of its
        // five answers without a GPU -- which matters because the two that look
        // identical on screen (not-yet-received and refused) are the two a live
        // session most needs told apart.
        SceneElementFacts facts;
        facts.present = entry != nullptr;
        if (entry != nullptr) {
            facts.alreadyBound = entry->sunFaceBuffer != nullptr;
            facts.triangleCount = entry->indexCount / 3;
            facts.topologyHash = entry->topologyHash;
        }

        switch (ClassifySunFaceBinding (map, facts)) {
            case SunFaceBinding::NotYetReceived:
                ++absent;
                continue;
            case SunFaceBinding::RefusedTriangleCount:
                entry->sunFaceBuffer.Release ();
                ++refusedCount;
                continue;
            case SunFaceBinding::RefusedTopologyHash:
                entry->sunFaceBuffer.Release ();
                ++refusedHash;
                continue;
            case SunFaceBinding::AlreadyBound:
                ++attached;
                continue;
            case SunFaceBinding::Attach:
                break;
        }

        Diligent::BufferDesc bd;
        bd.Name = "ArchViz sun study face maps";
        bd.Size = map.faces.size () * sizeof (SunFaceMap);
        bd.Usage = Diligent::USAGE_IMMUTABLE;
        bd.BindFlags = Diligent::BIND_SHADER_RESOURCE;
        bd.Mode = Diligent::BUFFER_MODE_STRUCTURED;
        bd.ElementByteStride = sizeof (SunFaceMap);
        const Diligent::BufferData bufferData { map.faces.data (), bd.Size };
        device->CreateBuffer (bd, &bufferData, &entry->sunFaceBuffer);
        if (entry->sunFaceBuffer != nullptr)
            ++attached;
    }

    impl_->sunElementsNamed = study.elements.size ();
    impl_->sunElementsAttached = attached;
    impl_->sunRefusedTriangleCount = refusedCount;
    impl_->sunRefusedTopologyHash = refusedHash;
    impl_->sunElementsAbsent = absent;
    // ⚠️ NAMED-BUT-NONE-ATTACHED IS A REFUSAL, NOT A QUIET SUCCESS, and it is the
    // case a picture cannot report: an untinted building looks exactly like a
    // building the sun never reached. Say it in one sentence a caller can print.
    // ⚠️ "NOT HERE YET" AND "REFUSED" ARE DIFFERENT ANSWERS AND MUST READ
    // DIFFERENTLY. An element the scene has not received is a bind waiting for
    // the next EndBatch, and it resolves itself; an element whose hash disagrees
    // never will, and needs a new study. Collapsing them into one sentence sends
    // a reader looking for a model mismatch that is not there -- or, worse, lets
    // them wait for a bind that is never coming.
    impl_->sunRejection.clear ();
    if (study.elements.empty ()) {
        impl_->sunRejection = "the study named no elements";
    }
    else if (attached == 0 && absent == study.elements.size ()) {
        impl_->sunRejection = "none of the study's " + std::to_string (absent) +
                              " element(s) have reached the viewer yet - the tint appears when the extraction "
                              "that carries them finishes";
    }
    else if (attached == 0) {
        impl_->sunRejection = "no element accepted the study - the viewer holds a different extraction of this "
                              "model than the study measured";
    }
    else if (attached < study.elements.size ()) {
        impl_->sunRejection = std::to_string (study.elements.size () - attached) + " of " +
                              std::to_string (study.elements.size ()) + " element(s) not tinted (" +
                              std::to_string (absent) + " not yet received, " +
                              std::to_string (refusedCount + refusedHash) + " refused)";
    }

    // ⚠️ REPORTED EVEN WHEN EVERYTHING WORKED. "Six named, six attached" and
    // "six named, two attached" are the difference between a study that is on
    // screen and a study that is on screen for a third of the building, and the
    // picture alone cannot tell them apart -- the untinted elements simply look
    // like elements the sun never reached.
    // ⚠️ LOGGED ONLY WHEN THE ANSWER CHANGES. AttachSunStudy runs at every
    // EndBatch, and during navigation those arrive continuously -- the first
    // version of this line wrote forty identical "6 of 6" entries into the one
    // log the whole viewer shares, which buried the events that matter and made
    // a healthy overlay look like a thrashing one. A study that binds 0 of 6 and
    // then 6 of 6 still gets both lines, because that transition is the thing
    // worth seeing.
    const bool changed = attached != previousAttached || absent != previousAbsent ||
                         refusedCount != previousRefusedCount || refusedHash != previousRefusedHash;
    if (!changed)
        return;

    ArchVizLog ("Diligent scene: sun study '" + impl_->sunStudyId + "' atlas " + std::to_string (impl_->sunAtlasWidth) +
                "x" + std::to_string (impl_->sunAtlasHeight) + ", " + std::to_string (attached) + " of " +
                std::to_string (study.elements.size ()) + " elements attached" +
                (absent > 0 ? ", " + std::to_string (absent) + " not yet received" : "") +
                (refusedHash > 0 ? ", " + std::to_string (refusedHash) +
                                       " refused (the viewer holds a different extraction of that element)"
                                 : "") +
                (refusedCount > 0 ? ", " + std::to_string (refusedCount) + " refused (triangle count mismatch)" : ""));
}

void DiligentScene::DrawSunStudyTint (Diligent::IDeviceContext* context, DiligentSceneConstants& constants,
                                      CullMode cull)
{
    if (impl_ == nullptr || context == nullptr || impl_->sunAtlasSRV == nullptr)
        return;
    if (impl_->sunElementsAttached == 0)
        return;

    // Built on first use and never retried after a failure -- the same shape and
    // the same reasons as the occlusion prepass. A user who never runs a sun
    // study never pays the HLSL compile.
    if (impl_->sunTintPso[0][0] == nullptr) {
        if (impl_->sunTintInitFailed)
            return;
        std::string initError;
        if (!CreateSunStudyPipeline (impl_->device, impl_->initColorFormat, impl_->initDepthFormat, initError)) {
            impl_->sunTintInitFailed = true;
            ArchVizLog ("Diligent scene: sun study tint unavailable (" + initError +
                        ") -- the model draws with ordinary shading");
            return;
        }
    }

    const int index = CullIndex (cull);
    const int depthIdx = impl_->sunDepthMode < uint32_t (kSunDepthModeCount) ? int (impl_->sunDepthMode) : 0;
    Diligent::IPipelineState* pso = impl_->sunTintPso[depthIdx][index];
    Diligent::IShaderResourceBinding* srb = impl_->sunTintSrb[index];
    if (pso == nullptr || srb == nullptr)
        return;

    // ⚠️ THE WHOLE CONSTANT BUFFER IS RE-UPLOADED WITH THE STUDY'S LANES FILLED.
    // `viewProj` must be byte-for-byte the one the shaded pass used, or the
    // depth-EQUAL test fails and the tint disappears; the caller hands its own
    // `constants` in for exactly that reason rather than this pass building a
    // fresh one from a camera it would have to be given separately.
    constants.sunStudyParams[0] = impl_->sunAtlasWidth > 0 ? 1.0f / float (impl_->sunAtlasWidth) : 0.0f;
    constants.sunStudyParams[1] = impl_->sunAtlasHeight > 0 ? 1.0f / float (impl_->sunAtlasHeight) : 0.0f;
    constants.sunStudyParams[2] = impl_->sunHoursMax;
    constants.sunStudyParams[3] = float (impl_->sunDebugMode);
    constants.sunStudyFilter[0] = impl_->sunFilterLo;
    constants.sunStudyFilter[1] = impl_->sunFilterHi;
    constants.sunStudyFilter[2] = impl_->sunQuantumHours;
    constants.sunStudyFilter[3] = impl_->sunFilterHide ? 1.0f : 0.0f;
    UploadConstants (context, impl_->constants, constants);

    if (Diligent::IShaderResourceVariable* atlas = srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_sunAtlas"))
        atlas->Set (impl_->sunAtlasSRV);

    context->SetPipelineState (pso);

    size_t draws = 0;
    // ⚠️ COUNTED PER FRAME, REPORTED FOR THE LIFETIME. An element the scene is
    // DRAWING whose side buffer is not bound is a frame where the model is on
    // screen and its tint is not -- which on a navigating camera is exactly one
    // dark or untinted flash. Skipping is right (drawing with a missing resource
    // is far worse); being unable to tell that it happened is not.
    size_t incomplete = 0;
    for (const Entry& e : impl_->elements) {
        if (e.vertexBuffer == nullptr || e.indexBuffer == nullptr)
            continue;
        if (e.sunFaceBuffer == nullptr) {
            ++incomplete;
            continue;
        }
        // ⚠️ THE SIDE BUFFER IS RE-SET AND THE SRB RE-COMMITTED PER ELEMENT. It
        // is the DYNAMIC variable in this layout; binding it once outside the
        // loop draws every element through the first one's face maps, which is
        // the same failure as the material ranges one layer up and looks just as
        // much like a result.
        if (Diligent::IShaderResourceVariable* faces =
                srb->GetVariableByName (Diligent::SHADER_TYPE_PIXEL, "g_sunFaceMaps"))
            faces->Set (e.sunFaceBuffer->GetDefaultView (Diligent::BUFFER_VIEW_SHADER_RESOURCE));
        context->CommitShaderResources (srb, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        BindMesh (context, e);
        // ⚠️ THE WHOLE ELEMENT IN ONE DRAW, NOT PER MATERIAL RANGE -- and that is
        // what makes SV_PrimitiveID the element-local triangle index the side
        // buffer is built for. A per-range draw would restart the primitive id
        // at zero inside each range while the buffer counts through the element,
        // so every range after the first would read the first range's tiles.
        // The tint owes nothing to the material anyway.
        const MaterialRange whole { -1, 0, e.indexCount };
        DrawEntryRange (context, e, whole);
        ++draws;
    }
    impl_->drawCalls += draws;
    ++impl_->sunTintFrames;
    impl_->sunTintElementsDrawn += draws;
    if (incomplete > 0)
        ++impl_->sunFramesSkippedIncompleteBinding;
}

void DiligentScene::SetSunStudyFilter (float lo, float hi, bool hide)
{
    if (impl_ == nullptr)
        return;
    impl_->sunFilterLo = (std::max) (0.0f, lo); // parenthesised: windows.h defines max
    // The slider's top is "9+": at it, no surface is cut off for having MORE.
    impl_->sunFilterHi = hi >= kSunHoursFilterOpenTop ? 1.0e9f : (std::max) (impl_->sunFilterLo, hi);
    impl_->sunFilterHide = hide;
}

} // namespace archviz
} // namespace geomsrv
