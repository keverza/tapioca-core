#ifndef EVP_ARCHVIZ_DILIGENTSHADERS_HPP
#define EVP_ARCHVIZ_DILIGENTSHADERS_HPP

// ArchViz/DiligentShaders — the mesh shader pair, in HLSL, embedded.
//
// The bgfx path compiled `Shaders/*.sc` with shaderc into `ShadersBin/*.bin.h`
// byte arrays. Diligent's D3D11 backend always has the HLSL compiler, so these
// are compiled at device-init time from the source below. That trades ~2 ms of
// startup for deleting an offline build step, a toolchain and a class of bug
// where the checked-in blob and the source disagree. When shipping wants
// precompiled DXBC, `ShaderCreateInfo::ByteCode` takes it and only this file
// changes.
//
// ⚠️ THIS IS NO LONGER A LINE-FOR-LINE PORT OF `fs_archviz_mesh.sc`. It was, and
// the port was faithful, and the result was reported live as "the building is
// there, materials roughly match, and the shading still looks flat". That report
// was correct and the shader was not broken: constant-ambient Lambert gives every
// surface of a given orientation exactly one value, so a wall is one flat colour
// from top to bottom and nothing indicates that the building has an inside, a
// recess or a soffit. Two things carry almost all of the difference, and both
// are here now:
//
//   HEMISPHERIC AMBIENT instead of a constant floor. Ambient light does not
//   arrive equally from every direction: the sky is bright and the ground is
//   dark, so an up-facing surface in shadow is much brighter than a down-facing
//   one. This is one lerp on the normal's z, and it is what makes soffits,
//   reveals and the undersides of balconies read.
//
//   A SHADOW MAP. Without it a building cannot occlude itself, so the courtyard
//   is as bright as the roof and there is nothing on the ground to say how tall
//   anything is.
//
// The bgfx shaders are NOT being kept in step with this. They are the old
// renderer, on the way out; when they go, this comment goes with them.
//
// Things about the HLSL that are easy to get wrong and silent when wrong:
//
//   ⚠️ THE MATRIX IS TRANSPOSED BY THE UPLOAD, DELIBERATELY. MatrixMath stores
//   row-major with row-vector semantics (bx's layout). An HLSL `float4x4` in a
//   cbuffer packs COLUMN-major by default, so those same 16 floats arrive as
//   the transpose -- which is exactly what makes the ordinary column-vector
//   `mul(matrix, vector)` below correct. Do not add a transpose on either side.
//   This applies to g_lightViewProj exactly as it does to g_viewProj.
//
//   ⚠️ ATTRIB2 READS AS RGBA EVEN THOUGH THE STRUCT SAYS "abgr". The vertex
//   field is a uint32 written as 0xAABBGGRR; on a little-endian machine its
//   bytes are R,G,B,A in memory, and a VT_UINT8 x4 normalised attribute maps
//   byte 0 to .x. So `input.color.rgb` is already red, green, blue. A "fix"
//   that swizzles to .bgr turns the debug cube's orange top blue.
//
//   ⚠️ NO MODEL MATRIX. Extraction emits world-space metres, so the world
//   normal IS the vertex normal. When a model matrix arrives, so must its
//   inverse-transpose for the normal -- a rotated normal is not a rotated
//   position.
//
//   ⚠️ EVERYTHING HERE IS LINEAR. The swap chain's render-target view is
//   _SRGB, so the hardware encodes on write. Adding a pow(1/2.2) in the shader
//   double-encodes and washes the whole image out -- which reads as "the new
//   lighting is too bright" rather than as a gamma bug.

#include <string>

namespace geomsrv {
namespace archviz {

// The cbuffer's C++ half. ⚠️ FIELD ORDER IS AN ABI with kArchVizMeshVS/PS
// below, exactly as Uniforms.hpp is with uniforms.sh. Append, never insert.
struct DiligentSceneConstants {
    float viewProj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // The same three vec4s as ArchVizParams, laid out identically so the
    // producer side is shared with the bgfx path.
    float sunDir[3] = { 0.0f, 0.0f, 1.0f };
    float ambient = 0.35f;
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float eyePos[3] = { 0.0f, 0.0f, 0.0f };
    // ⚠️ THE SPARE IS THE DEBUG VIEW SELECTOR, and it earns the slot. A live run
    // reported the cube as "flat, evenly lit", which is indistinguishable by eye
    // from four different faults: the sun never reaching the pixel shader, the
    // normals arriving wrong, the constant buffer being read at the wrong
    // offset, or the lighting being correct and simply hard to judge. Rendering
    // the shader's own inputs as colour separates all four in ONE look, and it
    // costs no extra PSO, no extra buffer and no extra draw.
    float debugView = 0.0f;

    // ---- appended for the shadow map and the hemispheric ambient ------------
    float lightViewProj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // x = world metres per shadow texel (the normal-offset scale), y = the
    // light's depth range in metres, z = 1 when the shadow map is valid and
    // bound, w = 1/resolution.
    float shadowParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Linear radiance, NOT sRGB. rgb, w = the sun's overall strength.
    float skyColor[4] = { 0.55f, 0.62f, 0.75f, 1.0f };
    float groundColor[4] = { 0.22f, 0.20f, 0.18f, 1.0f };

    // ---- appended for the selection silhouette (PLAT-RE41) ------------------
    // xy = how far one outline pixel is in NDC (2/width, 2/height) TIMES the
    // requested thickness in pixels; zw unused.
    //
    // ⚠️ IT IS A SCREEN-SPACE OFFSET, NOT A WORLD ONE, and that is the whole
    // reason it needs the viewport size. A hull expanded by world metres is
    // hairline-thin on a site model and swallows a door handle, so the thickness
    // would have to be retuned for every zoom level -- which is the same as not
    // working.
    //
    // ⚠️ `z` IS NOW THE RENDER QUALITY (PLAT-RE126): 0 = Fast, 1 = Realistic. It
    // rides in an already-declared float4's spare lane deliberately -- appending
    // an eighth float4 would change the cbuffer's size, and the static_assert
    // below plus every PSO built against this layout would have to move with it
    // for one boolean.
    //
    // ⚠️ `w` IS THE DRAW RANGE'S PERCEPTUAL ROUGHNESS, 0 = mirror, 1 = matte,
    // and it takes the last spare lane for the same reason `z` took the one
    // before it. It is UPLOADED PER RANGE alongside `baseColor` -- roughness is
    // a property of the material being submitted, not of the frame, so hoisting
    // it out of the range loop paints the whole model in the last material's
    // finish, which is the failure MeshGroups exists to prevent wearing a
    // different hat.
    float outlineParams[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    // ---- appended for the HDR environment (PLAT-RE51) -----------------------
    //
    // Nine order-2 spherical-harmonic coefficients, xyz = RGB, w unused. They
    // ARE the entire diffuse contribution of the sky -- see
    // ArchViz/EnvironmentLighting, where they are produced and tested.
    //
    // ⚠️ NINE float4s FOR NINE float3s, WASTING A LANE EACH. HLSL packs a
    // cbuffer array with each element on its own 16-byte boundary, so a
    // `float3 g_sh[9]` in the shader is NOT 108 bytes -- it is 144 with the
    // padding invisible on this side. Declaring float4 here makes the padding
    // explicit and the two sides impossible to disagree about; packing them
    // tightly is what silently shifts every coefficient after the first.
    float sh[9][4] = {};

    // x = intensity multiplier, y = rotation about Z in RADIANS,
    // z = 1 when a sky is loaded and bound, w = the texture's mip count.
    float envParams[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

    // ---- appended for the second material channel (PLAT-RE51) ---------------
    //
    // x = the draw range's specular reflectance, 0..1, UPLOADED PER RANGE
    // beside `baseColor` exactly as roughness is.
    // y = how much analytic sun survives when an HDR sky is active, 0..1. ⚠️ A
    // FRAME value sharing a float4 with a PER-RANGE one, which is safe only
    // because the whole struct is re-uploaded per range -- set y once before the
    // range loop and it rides along. Do not "optimise" that upload out.
    // z = the draw range's classifier-provided metalness, 0..1. w spare.
    //
    // ⚠️ A WHOLE float4 FOR ONE FLOAT, AND THAT IS THE DOCUMENTED CHOICE. The
    // note on `outlineParams` records the previous decision to subdivide a
    // spare lane; the fallback stated there for a SECOND per-material channel
    // was to append rather than subdivide again, because the static_assert then
    // forces both sides of the ABI to move together. This is that fallback
    // being taken. The three spare lanes are the next channel's, at no cost.
    float materialParams[4] = { 0.5f, 0.0f, 0.0f, 0.0f };

    // ---- appended for the sky BACKGROUND (PLAT-RE51) ------------------------
    //
    // The camera's view ray, as three world vectors: a pixel at NDC (x, y) looks
    // along `forward + x * right + y * up`. `right` and `up` already carry the
    // half-extent of the projection plane, so the shader needs no FOV and no
    // aspect of its own.
    //
    // ⚠️ A RAY BASIS RATHER THAN AN INVERSE VIEW-PROJECTION, on purpose. The
    // obvious alternative is to unproject the far plane, which needs a 4x4
    // inverse this codebase does not have and would have to grow and test for
    // one shader. The basis comes straight out of MatrixMath's `CameraBasis`,
    // which is already the single derivation the pick ray and the camera share.
    //
    // ⚠️ IN PARALLEL PROJECTION right AND up ARE ZERO, and that is correct, not
    // a missing case: an orthographic camera's rays are all parallel, so every
    // pixel of an infinitely distant sky looks the same direction.
    float envRayRight[4] = {};
    float envRayUp[4] = {};
    float envRayForward[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

    // ---- appended for the Realistic grading controls (PLAT-RE51) ------------
    //
    // x = exposure, the pre-scale into the ACES curve. Was hard-coded 0.6.
    // y = reflectance, a multiplier on BOTH specular terms. 1 = physical.
    // z = roughness bias, ADDED to every material's roughness before clamping.
    //     Negative makes the whole model glossier.
    // w = ambient occlusion intensity (RE51.C3), 0 = off. ⚠️ IT IS ALSO THE
    //     SHADER'S ONLY GATE on `g_ambientOcclusion`: HLSL cannot ask whether a
    //     texture is bound, and an unbound Load returns ZERO, which reads as
    //     fully occluded and would render the model black. Draw sets this to 0
    //     whenever no occlusion was prepared.
    //
    // ⚠️ THESE EXIST BECAUSE ARCHICAD'S SURFACE DATA IS NOT PBR DATA, and no
    // amount of correct shading fixes that on its own. This project's pool was
    // measured at 0.72-0.8% `shining` on nearly every painted surface, which is
    // a perceptual roughness of 0.99 -- a GGX lobe so broad it has no visible
    // highlight. So Realistic renders almost identically to Fast, which is
    // exactly what was reported, and the renderer is not at fault: it is
    // faithfully drawing a model whose materials all claim to be matte.
    //
    // ⚠️ THE REAL FIX IS THE ARCHITECTURAL MATERIAL PRESETS in
    // PROGRAM-RENDERER-TASK.md section 3 -- a table that recognises glass,
    // metal, concrete and paint and gives each a plausible PBR description
    // instead of trusting three legacy Blinn-Phong numbers. These three sliders
    // are the interim: they let the look be found by eye first, so the presets
    // are calibrated against something seen rather than guessed at.
    // ⚠️ 1.2, AND IT MOVED WITH THE TONE CURVE, NOT AS A BRIGHTNESS PREFERENCE.
    // The old 0.6 was the pre-scale for Narkowicz's per-channel ACES; AcesFitted
    // replaced that curve, and 1.2 is the pre-scale at which the two agree on
    // neutral grey to within 0.002 across the whole range. Changing one without
    // the other changes the exposure of every image ever compared against these.
    float gradeParams[4] = { 1.2f, 1.0f, 0.0f, 0.0f };

    // ---- appended for RE51.B6, the GGX environment prefilter ----------------
    //
    // x = the perceptual roughness this prefilter pass is convolving for,
    // y = how many GGX samples to take, z = the SOURCE map's width in texels,
    // w = its height. ⚠️ THESE ARE READ BY ONE SHADER ONLY -- the offline
    // prefilter in ArchViz/EnvironmentMap, which runs at LOAD time and never
    // during a frame. The scene's own draw path leaves them at their defaults
    // and never looks at them.
    //
    // ⚠️ THEY RIDE THE SCENE cbuffer RATHER THAN A SECOND ONE ON PURPOSE. The
    // prefilter shader needs `EnvUv`, which reads `g_envParams.y`; giving it a
    // private constant buffer would mean either a second copy of that lookup or
    // two buffers bound to one tiny pass. Sharing the declaration is what keeps
    // exactly ONE equirectangular convention in the tree (see EnvUv's ⚠️).
    float prefilterParams[4] = { 0.0f, 64.0f, 2048.0f, 1024.0f };

    // ---- appended for RE51.B9, white balance --------------------------------
    //
    // rgb = LINEAR per-channel gains applied immediately before the exposure
    // pre-scale and the tone curve; w spare. 1,1,1 is the identity, which is
    // what every image rendered before this existed used.
    //
    // ⚠️ IT IS A GAIN, NOT A TEMPERATURE. The Kelvin-and-tint arithmetic lives
    // on the CPU in ArchViz/AutoExposure, where it is tested offline against
    // published chromaticities; the shader receives the result. Putting a
    // black-body approximation in HLSL would make it untestable and would give
    // the HUD and the renderer two chances to disagree about what 5500 K is.
    float whiteBalance[4] = { 1.0f, 1.0f, 1.0f, 0.0f };

    // ---- appended for RE51.C2, real motion vectors ---------------------------
    //
    // LAST frame's view-projection, so the G-buffer pass can put this frame's
    // world position where the previous frame's camera would have put it.
    //
    // ⚠️ ON THE FIRST FRAME IT IS THIS FRAME'S, DELIBERATELY. An identity or
    // uninitialised previous matrix produces an enormous motion vector for every
    // pixel, and the effects downstream then either throw their whole history
    // away -- survivable -- or reproject from far off-screen, which is a flash of
    // garbage on the first frame after every resize. Seeding it with the current
    // matrix makes the first frame's motion exactly zero, which is also true.
    //
    // ⚠️ IT DESCRIBES THE CAMERA, NOT THE MODEL. Extraction emits world-space
    // vertices and there is no per-element transform anywhere in this renderer
    // (see kArchVizMeshVS's "NO MODEL MATRIX"), so an element that MOVES gets a
    // new vertex buffer rather than a new matrix, and its motion reads zero for
    // the frame the new geometry arrives. That is C2's remaining half, recorded
    // rather than hidden: a re-extracted element can ghost for a frame in any
    // effect that trusts history.
    float prevViewProj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    // ---- appended for the HDR scene-colour target (RE51.C7 prerequisite) -----
    //
    // x = 1 when the frame renders into the offscreen RGBA16_FLOAT target and
    // Grade() must be SKIPPED, leaving raw scene-referred radiance for the
    // resolve pass to tone-map in one place. 0 = the existing LDR path, where
    // each shader tone-maps individually into the swap chain's sRGB view.
    //
    // ⚠️ THIS IS THE FIELD THAT MOVES TONE MAPPING OUT OF TWO SHADERS AND INTO
    // ONE. The mesh PS and the env background PS both branch on it; the resolve
    // PS applies Grade() unconditionally. With this off, the renderer behaves
    // exactly as it did before the HDR target existed, which is the property
    // that makes the increment verifiable in isolation.
    float frameControl[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // Current UNJITTERED view-projection. Visible geometry uses `viewProj`, but
    // temporal reprojection must not report the Halton sample offset as motion.
    float motionViewProj[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
};

// What the viewport presents. ⚠️ THESE VALUES ARE AN ABI with the `if` ladder
// in kArchVizMeshPS, the G-buffer dispatch in DiligentViewport, and
// `_DEBUG_VIEWS` in Diagnostics/Commands/DiligentViewportSmoke/command.py.
enum class DiligentDebugView : int {
    Final = 0,     // the ordinary shaded result
    Normals = 1,   // world normal as colour: n * 0.5 + 0.5
    Lit = 2,       // the lighting term alone, grey. FLAT GREY HERE MEANS THE
                   // SUN REALLY IS NOT REACHING THE SHADER.
    BaseColor = 3, // vertex colour x material colour, unlit
    SunVector = 4, // the sun direction the shader HAS, as a flat colour --
                   // black means the constant buffer never arrived
    Shadow = 5,    // the shadow term alone: white lit, black occluded. ALL
                   // WHITE means the map is not bound or the frustum missed
                   // the model; ALL BLACK means the depth comparison is
                   // inverted, which no other view distinguishes.
    Roughness = 6, // the per-range roughness as grey: black mirror, white
                   // matte. ⚠️ IT EXISTS BECAUSE A UNIFORMLY WHITE IMAGE HERE
                   // IS A REAL AND LIKELY ANSWER -- it means every surface in
                   // the project reports GetShining()==0, so the GGX lobe has
                   // nothing to vary and `Realistic` is correctly matte rather
                   // than broken. Nothing else separates "the material channel
                   // is flat" from "the specular never reached the shader".
    GBufferNormals = 7,
    GBufferDepth = 8,
    AmbientOcclusion = 9,
    GBufferAlbedo = 10,
    GBufferRoughness = 11,
    GBufferMaterialData = 12,
    // ⚠️ RE51.C2's ACCEPTANCE ASKS FOR THIS BY NAME: "a debug view shows them
    // before any consumer is enabled". Motion vectors are the one G-buffer
    // channel whose correctness cannot be judged from the shaded image at all --
    // a sign error, an axis swap or a stale previous matrix all produce a
    // perfectly ordinary-looking frame, and only show up later as a temporal
    // effect smearing the wrong way.
    MotionVectors = 13,
};

static_assert (sizeof (DiligentSceneConstants) ==
                   64 + 48 + 64 + 48 + 16 + 9 * 16 + 16 + 16 + 48 + 16 + 16 + 16 + 64 + 16 + 64,
               "the cbuffer is three float4x4s, seven float4s, the 9-element SH array, "
               "the environment parameters, the material parameters, the three "
               "view-ray vectors, the grading parameters, the prefilter parameters "
               "the white balance, the previous view-projection, and the frame "
               "control flag, and HLSL will read it that way");

// The cbuffer declaration, ONE COPY, prepended to each shader by
// ArchVizShaderSource below.
//
// ⚠️ ALL THREE STAGES MUST DECLARE IT IDENTICALLY. HLSL packs a cbuffer by
// declaration order, so a stage that omits or reorders a field reads every
// later field at the wrong offset -- and reads it SUCCESSFULLY, with no
// compiler complaint and no validation error. Three hand-maintained copies is
// exactly the shape that drifts, so there is one.
//
// (It is concatenated at runtime rather than by the preprocessor: a raw string
// literal inside a macro definition is legal C++ but has a history of MSVC
// bugs, and this is not worth spending one on.)
constexpr const char* kArchVizCBuffer = R"hlsl(
cbuffer ArchVizConstants
{
    float4x4 g_viewProj;
    float4   g_sunAndAmbient;   // xyz = toward the sun (normalised), w = ambient
    float4   g_baseColor;       // per draw range: rgb surface colour, a opacity
    float4   g_eyePos;          // xyz = camera world position, w = debug view
    float4x4 g_lightViewProj;
    float4   g_shadowParams;    // x texel metres, y depth range, z enabled, w 1/res
    float4   g_skyColor;
    float4   g_groundColor;
    float4   g_outline;         // xy = the silhouette offset in NDC per vertex
                                // z  = render quality, w = range roughness
    float4   g_sh[9];           // order-2 SH irradiance, xyz = RGB
    float4   g_envParams;       // x intensity, y rotation rad, z enabled, w mips
    float4   g_materialParams;  // per draw range: x = specular, y = sun weight, z = metalness
    float4   g_envRayRight;     // the view ray basis: dir = forward + x*right
    float4   g_envRayUp;        //                         + y*up, at NDC (x, y)
    float4   g_envRayForward;
    float4   g_gradeParams;     // x exposure, y reflectance, z roughness bias,
                                // w ambient occlusion intensity AND its gate
    float4   g_prefilterParams; // LOAD-TIME ONLY: x roughness, y samples,
                                // zw source size. See DiligentSceneConstants.
    float4   g_whiteBalance;    // rgb linear gains, applied before the exposure
    float4x4 g_prevViewProj;    // RE51.C2: last frame's camera, for motion
    float4   g_frameControl;    // x = 1 for HDR output (skip Grade, resolve pass tone-maps)
                                // y = 1 when g_hdrCoverage is TAA-resolved (coverage in RED,
                                //     not ALPHA -- see kArchVizCoveragePS)
    float4x4 g_motionViewProj;  // RE51.C8: current camera without projection jitter
};
)hlsl";

constexpr const char* kArchVizMeshVS = R"hlsl(
struct VSInput
{
    float3 position : ATTRIB0;
    float3 normal   : ATTRIB1;
    float4 color    : ATTRIB2;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
    // ---- RE51.C2 ---------------------------------------------------------
    // ⚠️ THE CLIP POSITION IS CARRIED SEPARATELY EVEN THOUGH SV_POSITION EXISTS.
    // By the time the pixel shader sees `position` the hardware has already
    // divided by w and mapped it to PIXELS, so the NDC a motion vector needs is
    // not recoverable from it without the viewport size and the w that was
    // thrown away. Interpolating the clip position costs two more lanes and is
    // exact.
    //
    // ⚠️ AND THE FORWARD PIXEL SHADER DOES NOT DECLARE THESE. That is legal and
    // intended: a D3D pixel shader's input signature only has to be a SUBSET of
    // the vertex shader's output. Adding them here therefore costs the forward
    // path two interpolators and nothing else, and keeps ONE vertex shader
    // feeding both passes -- which is what stops the G-buffer and the shaded
    // image from ever disagreeing about where a triangle is.
    float4 currClip : CURRCLIP;
    float4 prevClip : PREVCLIP;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    float4 world = float4 (vsIn.position, 1.0);
    psIn.position = mul (g_viewProj, world);
    psIn.worldPos = vsIn.position;   // already world space
    psIn.normal   = vsIn.normal;     // identity model matrix, so no normal matrix
    psIn.color    = vsIn.color;
    // Visible position is jittered; motion is not. Otherwise a stationary edge
    // reports the Halton sample delta as movement and TAA chases its own jitter.
    psIn.currClip = mul (g_motionViewProj, world);
    psIn.prevClip = mul (g_prevViewProj, world);
}
)hlsl";

// The shadow pass. Position only -- no normal, no colour, no pixel shader --
// because the only output is depth.
//
// ⚠️ IT SHARES THE VERTEX BUFFER AND THEREFORE THE FULL INPUT LAYOUT. A
// position-only layout over a 28-byte vertex would need its own buffer or a
// stride the layout does not describe; declaring all three attributes and
// ignoring two costs nothing and cannot desynchronise.
constexpr const char* kArchVizShadowVS = R"hlsl(
struct VSInput
{
    float3 position : ATTRIB0;
    float3 normal   : ATTRIB1;
    float4 color    : ATTRIB2;
};

void main (in VSInput vsIn, out float4 position : SV_POSITION)
{
    position = mul (g_lightViewProj, float4 (vsIn.position, 1.0));
}
)hlsl";

// The SILHOUETTE pass's vertex shader: the mesh, pushed outward in SCREEN space
// along its projected normal.
//
// It is the inverted-hull outline. Drawn with the cull mode INVERTED against the
// visible pass -- so only the faces pointing away from the camera -- and with
// depth testing on but no depth write, the expanded hull is hidden everywhere the
// object itself is nearer and survives only as a ring a few pixels wide around
// the object's silhouette. That ring is the outline.
//
// ⚠️ THE OFFSET IS IN CLIP SPACE, MULTIPLIED BY w, AND BOTH HALVES MATTER.
// Offsetting in world space along the normal gives a thickness that scales with
// the model and with the zoom, so one constant is a hairline on a site plan and
// a blob on a door handle. Offsetting the CLIP position by `ndcPerPixel * w`
// makes the perspective divide cancel the w back out, leaving exactly the
// requested number of PIXELS at any distance.
//
// ⚠️ THE NORMAL IS ROTATED BY g_viewProj's UPPER 3x3, WHICH IS ONLY CORRECT
// BECAUSE THERE IS NO MODEL MATRIX AND NO NON-UNIFORM SCALE (see the ⚠️ at the
// top of this file). The projection's own perspective term is deliberately
// ignored: the direction to push a silhouette in is the normal's SCREEN
// direction, and normalising xy after the rotation is what supplies it.
constexpr const char* kArchVizOutlineVS = R"hlsl(
struct VSInput
{
    float3 position : ATTRIB0;
    float3 normal   : ATTRIB1;
    float4 color    : ATTRIB2;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

void main (in VSInput vsIn, out PSInput psIn)
{
    float4 clipPos = mul (g_viewProj, float4 (vsIn.position, 1.0));
    float3 clipNormal = mul ((float3x3) g_viewProj, vsIn.normal);

    // A face seen exactly edge-on projects to a zero-length screen normal, and
    // normalize(0) is a NaN that removes the whole triangle. Leaving it
    // unexpanded is right: an edge-on face contributes nothing to the silhouette.
    float2 dir = clipNormal.xy;
    float len = length (dir);
    if (len > 1e-6)
        clipPos.xy += (dir / len) * g_outline.xy * clipPos.w;

    psIn.position = clipPos;
    psIn.worldPos = vsIn.position;
    psIn.normal   = vsIn.normal;
    psIn.color    = vsIn.color;
}
)hlsl";

// A FLAT COLOUR, straight out of g_baseColor. Three passes share it and the
// sharing is deliberate:
//
//   the PICK pass    writes the element's id, unpacked into rgb by the caller
//   the OUTLINE pass writes the silhouette colour
//   the WIREFRAME    writes the line colour
//
// ⚠️ IT PAIRS WITH kArchVizMeshVS FOR PICKING, NOT WITH A VERTEX SHADER OF ITS
// OWN. The id pass needs only SV_POSITION, but reusing the mesh VS means the two
// passes cannot disagree about where a vertex lands -- and a pick pass that
// projects even slightly differently from the visible one resolves clicks near a
// silhouette to the element BEHIND the one the user is pointing at, which reads
// as "picking is a bit off" rather than as two projections.
//
// ⚠️ NO LIGHTING, NO GAMMA, NO ALPHA BLEND, NO MSAA IN THE PICK PASS. An id is
// not a colour: any filtering blends element 3 and element 260 into element 131,
// which exists, is plausible and is wrong. That PSO enforces the last two; this
// enforces the rest by having nothing else to say. The outline and wireframe
// PSOs DO blend, and pass an alpha in g_baseColor.a for it.
constexpr const char* kArchVizFlatPS = R"hlsl(
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    // ⚠️ THE ALPHA IS CARRIED THROUGH, AND THE PICK PASS RELIES ON IT BEING 1.
    // For picking, the caller has already unpacked the id into g_baseColor's
    // three channels as byte/255 and set a=1, and the target is RGBA8_UNORM, so
    // this round-trips exactly. For the outline and wireframe passes the alpha is
    // the line's own, and those PSOs blend with it.
    psOut.color = g_baseColor;
}
)hlsl";

// The deferred inputs are written in one opaque pass. Albedo stays in the
// source material space, roughness is the value the forward GGX path consumes,
// and material data reserves the standard PBR lanes for roughness, metalness,
// specular reflectance and opacity. Metalness is zero until the classifier owns
// that decision.
constexpr const char* kArchVizGBufferPS = R"hlsl(
struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
    float4 currClip : CURRCLIP;
    float4 prevClip : PREVCLIP;
};

struct PSOutput
{
    float4 normal       : SV_TARGET0;
    float4 albedo       : SV_TARGET1;
    float  roughness    : SV_TARGET2;
    float4 materialData : SV_TARGET3;
    float2 motion       : SV_TARGET4;
};

void main (in PSInput psIn, out PSOutput psOut)
{
    float4 albedo = psIn.color * g_baseColor;
    float roughness = clamp (g_outline.w + g_gradeParams.z, 0.045, 1.0);

    // Match the visible two-sided shader exactly. DiligentFX expects the normal
    // of the surface being shaded; feeding an imported back-face normal sends
    // the reflection ray through the receiver instead of above it.
    float3 normal = normalize (psIn.normal);
    float3 viewDir = normalize (g_eyePos.xyz - psIn.worldPos);
    if (dot (normal, viewDir) < 0.0)
        normal = -normal;
    psOut.normal = float4 (normal, 1.0);
    psOut.albedo = albedo;
    psOut.roughness = roughness;
    psOut.materialData = float4 (roughness, saturate (g_materialParams.z), saturate (g_materialParams.x),
                                 saturate (albedo.a));

    // ---- RE51.C2: the motion vector ---------------------------------------
    //
    // ⚠️ IT IS STORED IN NDC UNITS AND IT IS CURRENT MINUS PREVIOUS. That is
    // DiligentFX's convention and it is NOT guessable -- it was read out of
    // SSAO_ComputeTemporalAccumulation.fx, which computes
    //
    //     PrevLocation = Position.xy - Motion * F3NDC_XYZ_TO_UVD_SCALE.xy * ViewportSize
    //
    // with F3NDC_XYZ_TO_UVD_SCALE = (0.5, -0.5, 1.0) under D3D. Substituting the
    // pixel-from-NDC mapping shows that expression is exact only when the stored
    // value is (currNDC - prevNDC). ⚠️ THE SIGN IS THE WHOLE THING: reversed, a
    // temporal effect reprojects the wrong way and every moving edge smears in
    // the direction it came FROM, which reads as motion blur rather than as a
    // bug.
    //
    // ⚠️ THE PERSPECTIVE DIVIDE IS PER-PIXEL AND HAS TO BE. Interpolating an
    // already-divided NDC is affine in screen space, which is precisely what the
    // hardware's perspective correction exists to avoid; on a floor plane
    // running to the horizon the error is enormous.
    float2 currNdc = psIn.currClip.xy / max (psIn.currClip.w, 1e-6);
    float2 prevNdc = psIn.prevClip.xy / max (psIn.prevClip.w, 1e-6);
    psOut.motion = currNdc - prevNdc;
}
)hlsl";

constexpr const char* kArchVizFullScreenVS = R"hlsl(
void main (uint vertexId : SV_VertexID, out float4 position : SV_POSITION)
{
    float2 uv = float2 ((vertexId << 1) & 2, vertexId & 2);
    position = float4 (uv * float2 (2.0, -2.0) + float2 (-1.0, 1.0), 0.0, 1.0);
}
)hlsl";

// The frame's nearest and farthest depth, reduced into two uints. See
// ArchViz/DiligentDepthRange.hpp for why the depth view cannot use a fixed ramp.
//
// ⚠️ THE ATOMICS ARE ON THE FLOAT'S BIT PATTERN, AND THAT IS EXACT, NOT A
// SHORTCUT. IEEE-754 orders NON-NEGATIVE floats identically to their bit
// patterns read as unsigned ints, and a depth buffer holds nothing else -- so
// InterlockedMin on asuint(depth) is the true minimum, not an approximation of
// it. HLSL has no InterlockedMin for floats, so the alternative is a much
// slower two-pass reduction through a scratch texture.
constexpr const char* kArchVizDepthRangeCS = R"hlsl(
Texture2D<float> g_depth;
RWBuffer<uint>   g_depthRangeOut;

groupshared uint gsMinBits;
groupshared uint gsMaxBits;

[numthreads (8, 8, 1)]
void main (uint3 threadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0)
    {
        gsMinBits = 0xFFFFFFFF;
        gsMaxBits = 0;
    }
    GroupMemoryBarrierWithGroupSync ();

    uint width, height;
    g_depth.GetDimensions (width, height);
    if (threadId.x < width && threadId.y < height)
    {
        float depth = g_depth.Load (int3 (threadId.xy, 0));
        // ⚠️ THE CLEARED BACKGROUND IS EXCLUDED. It sits at the far plane, so
        // admitting it would peg the maximum at 20 km on every frame that shows
        // any sky at all -- which is every exterior frame, and would put the
        // ramp straight back where it started.
        if (depth < 1.0)
        {
            uint bits = asuint (depth);
            InterlockedMin (gsMinBits, bits);
            InterlockedMax (gsMaxBits, bits);
        }
    }
    GroupMemoryBarrierWithGroupSync ();

    // ⚠️ ONE ATOMIC PAIR PER GROUP, NOT PER PIXEL. A 1490x738 viewport is 1.1M
    // pixels; hammering two addresses that many times serialises the whole
    // dispatch. Reducing in groupshared first cuts it to ~17k.
    if (groupIndex == 0 && gsMinBits <= gsMaxBits)
    {
        InterlockedMin (g_depthRangeOut[0], gsMinBits);
        InterlockedMax (g_depthRangeOut[1], gsMaxBits);
    }
}
)hlsl";

// The raw per-frame range, eased into the one the shader actually reads.
//
// ⚠️ WITHOUT THIS THE RAMP IS CORRECT AND UNUSABLE. The raw min/max is a
// property of whatever is on screen THIS frame, so orbiting re-fits it every
// frame and the same wall changes shade continuously; worse, one pixel of
// geometry passing near the camera collapses the near end and the whole
// gradient visibly flips. Both were reported from the first live run. Easing
// the range keeps the auto-fit -- which is the thing that made the view legible
// at all -- while making it settle instead of chase.
constexpr const char* kArchVizDepthRangeSmoothCS = R"hlsl(
RWBuffer<uint> g_depthRangeRaw;
RWBuffer<uint> g_depthRangeSmooth;

[numthreads (1, 1, 1)]
void main ()
{
    uint rawMinBits = g_depthRangeRaw[0];
    uint rawMaxBits = g_depthRangeRaw[1];
    // Nothing measured (an empty frame, or a resize between the two passes):
    // HOLD the previous range rather than resetting it. Resetting is what makes
    // the picture jump for one frame and reads as a flicker.
    if (rawMinBits > rawMaxBits)
        return;

    float rawMin = asfloat (rawMinBits);
    float rawMax = asfloat (rawMaxBits);

    uint prevMinBits = g_depthRangeSmooth[0];
    uint prevMaxBits = g_depthRangeSmooth[1];
    if (prevMinBits > prevMaxBits)
    {
        // First measured frame: adopt outright. Easing up from a seeded value
        // would spend a second of wall time crossing a 20 km frustum.
        g_depthRangeSmooth[0] = rawMinBits;
        g_depthRangeSmooth[1] = rawMaxBits;
        return;
    }

    // ⚠️ FIXED RATE, NOT FRAME-RATE CORRECTED, AND DELIBERATELY SO. A dt-aware
    // ease needs the frame time in the cbuffer and buys nothing here: the
    // viewport is vsynced, and the visible behaviour of "settles over about a
    // second" is what is wanted at 60 or at 30.
    const float k = 0.08;
    float newMin = lerp (asfloat (prevMinBits), rawMin, k);
    float newMax = lerp (asfloat (prevMaxBits), rawMax, k);
    g_depthRangeSmooth[0] = asuint (newMin);
    g_depthRangeSmooth[1] = asuint (newMax);
}
)hlsl";

// The sky behind the model: one full-screen triangle, no depth, drawn first.
//
// ⚠️ THE RAY IS BUILT IN THE VERTEX SHADER AND INTERPOLATED, which is exact
// rather than a shortcut. A perspective view ray is linear in NDC across the
// projection plane, so three corner rays interpolate to the correct direction at
// every pixel; only the normalise has to be per-pixel, because interpolating
// unit vectors does not preserve length.
constexpr const char* kArchVizEnvBackgroundVS = R"hlsl(
void main (uint vertexId : SV_VertexID, out float4 position : SV_POSITION, out float3 ray : TEXCOORD0)
{
    float2 uv = float2 ((vertexId << 1) & 2, vertexId & 2);
    float2 ndc = uv * float2 (2.0, -2.0) + float2 (-1.0, 1.0);
    // ⚠️ z = 0, NOT 1. Depth testing is OFF for this pass, but the value still
    // has to be inside the clip volume or the triangle is culled outright and
    // the sky silently never appears.
    position = float4 (ndc, 0.0, 1.0);
    ray = g_envRayForward.xyz + ndc.x * g_envRayRight.xyz + ndc.y * g_envRayUp.xyz;
}
)hlsl";

// ---- RE51.B6: the GGX prefilter that replaces the box mip chain ------------
//
// ⚠️ THIS IS THE WHOLE OF B6 ON THE GPU, AND IT RUNS AT LOAD TIME, NOT PER
// FRAME. The environment texture the model reflects used to be box-filtered --
// each mip the arithmetic mean of four texels of the one above. That is the
// wrong kernel by construction: a GGX lobe at roughness r is not a box, it is
// not even radially symmetric in the equirectangular plane, and a box mip blurs
// ACROSS the map's distorted poles as if they were flat. The visible
// consequence is exactly what was reported live -- a polished surface reflects
// a grey smear rather than a recognisable environment -- and no amount of
// correcting the roughness-to-mip mapping fixes it, because the mip being
// selected does not contain the right image at any level.
//
// So each mip is now RE-RENDERED by importance-sampling the GGX distribution
// for that mip's roughness, which is Karis's split-sum first half
// (s2013_pbs_epic_notes_v2.pdf) and a direct port of DiligentFX's own
// Shaders/PBR/private/PrefilterEnvMap.psh. Two deliberate differences from that
// file, both forced by this renderer's shape:
//
//   ⚠️ EQUIRECTANGULAR, NOT A CUBEMAP. DiligentFX renders six cube faces
//   through CubemapFace.vsh. This tree binds ONE Texture2D as a STATIC shader
//   variable at pipeline-creation time (see EnvironmentMap.hpp), and swapping
//   that for a TextureCube would change the resource layout of every mesh PSO
//   and every SRB built from one. The lobe integral does not care what
//   parameterisation the result is stored in; only the texel solid angle does,
//   and that is what OmegaP below computes for a sphere map instead of a cube.
//
//   ⚠️ NO BRDF LUT. DiligentFX pairs this with PrecomputeBRDF.psh and samples
//   the table; the mesh shader here uses Karis's ANALYTIC fit to the same
//   table, which RE51.B4 already landed and measured. A LUT would be a third
//   texture binding for a difference below the precision of everything around
//   it. The analytic fit is named at its call site as the thing a LUT would
//   replace; that trade is unchanged by this unit.
//
// ⚠️ IT READS A DIFFERENT TEXTURE FROM THE ONE IT WRITES. `g_envSource` is the
// box-mipped upload; the destination is the bound environment map's mip `i`.
// Reading and writing one texture would be a hazard the D3D11 runtime resolves
// by silently unbinding the SRV, which produces a BLACK sky and no message.
constexpr const char* kArchVizEnvPrefilterVS = R"hlsl(
void main (uint vertexId : SV_VertexID, out float4 position : SV_POSITION, out float2 uv : TEXCOORD0)
{
    // The same full-screen triangle as kArchVizFullScreenVS, with the uv kept
    // rather than discarded: this pass needs the DESTINATION texel's position
    // in the equirectangular plane to know which direction it is filtering.
    uv = float2 ((vertexId << 1) & 2, vertexId & 2);
    position = float4 (uv * float2 (2.0, -2.0) + float2 (-1.0, 1.0), 0.0, 1.0);
}
)hlsl";

constexpr const char* kArchVizEnvPrefilterPS = R"hlsl(
Texture2D    g_envSource;
SamplerState g_envSource_sampler;

// Hammersley's low-discrepancy sequence. `reversebits` is a shader-model-5
// intrinsic, which the D3D11 backend at feature level 11 always has.
float2 Hammersley2D (uint i, uint n)
{
    return float2 (float (i) / float (n), float (reversebits (i)) * 2.3283064365386963e-10);
}

// Karis's GGX half-vector importance sample, in the tangent frame of `n`.
float3 ImportanceSampleGGX (float2 xi, float perceptualRoughness, float3 n)
{
    float alpha = perceptualRoughness * perceptualRoughness;
    float a2 = alpha * alpha;

    float phi = 6.28318530718 * xi.x;
    float cosTheta = sqrt (saturate ((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y)));
    float sinTheta = sqrt (saturate (1.0 - cosTheta * cosTheta));

    float3 h = float3 (sinTheta * cos (phi), sinTheta * sin (phi), cosTheta);
    float3 up = abs (n.z) < 0.999 ? float3 (0.0, 0.0, 1.0) : float3 (1.0, 0.0, 0.0);
    float3 tx = normalize (cross (up, n));
    float3 ty = cross (n, tx);
    return tx * h.x + ty * h.y + n * h.z;
}

void main (float4 position : SV_POSITION, float2 uv : TEXCOORD0, out float4 color : SV_TARGET)
{
    // ⚠️ N == V == R, THE STANDARD SPLIT-SUM ASSUMPTION, and it is an
    // approximation rather than a simplification: a prefiltered map is indexed
    // by ONE direction, so the view-dependent stretch of a grazing GGX lobe
    // cannot be stored. Every real-time IBL makes this trade; it is why grazing
    // reflections are slightly too round everywhere, including in UE5.
    float3 n = normalize (EnvDir (uv));

    float perceptualRoughness = g_prefilterParams.x;
    uint  numSamples = uint (g_prefilterParams.y);
    float srcWidth = max (g_prefilterParams.z, 1.0);
    float srcHeight = max (g_prefilterParams.w, 1.0);
    float srcMips = max (g_envParams.w, 1.0);

    float alpha = perceptualRoughness * perceptualRoughness;
    float a2 = max (alpha * alpha, 1e-8);

    float3 sum = float3 (0.0, 0.0, 0.0);
    float weight = 0.0;
    for (uint i = 0u; i < numSamples; ++i)
    {
        float2 xi = Hammersley2D (i, numSamples);
        float3 h = ImportanceSampleGGX (xi, perceptualRoughness, n);
        float3 l = 2.0 * dot (n, h) * h - n;

        float ndotl = saturate (dot (n, l));
        float ndoth = saturate (dot (n, h));
        if (ndotl <= 0.0 || ndoth <= 0.0)
            continue;

        // ⚠️ THE MIP BIAS IS NOT COSMETIC, IT IS WHAT KILLS THE FIREFLIES. With
        // a few hundred samples spread over a wide lobe, a single texel holding
        // the sun's disc -- thousands of times brighter than its neighbours --
        // lands in some output texels and not others, and the result is a rash
        // of bright dots that looks like corrupt memory. Choosing the source mip
        // whose texel solid angle matches the sample's own makes each sample an
        // AVERAGE of the region it represents instead of one lucky texel.
        // (placeholderart.wordpress.com, "Runtime environment map filtering".)
        //
        // vdoth == ndoth here because v == n, so the GGX sample PDF reduces to
        // D * ndoth / (4 * vdoth) == D / 4.
        float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
        float d = a2 / max (3.14159265359 * denom * denom, 1e-8);
        float pdf = max (d * 0.25, 1e-6);
        float omegaS = 1.0 / (float (numSamples) * pdf);

        // The solid angle of ONE SOURCE TEXEL at this sample's latitude. An
        // equirectangular texel spans 2pi/width in longitude and pi/height in
        // latitude, and shrinks toward the poles by sin(theta) -- which is the
        // whole reason a box mip chain is wrong for this map.
        float sinTheta = max (sqrt (saturate (1.0 - l.z * l.z)), 1e-4);
        float omegaP = (6.28318530718 / srcWidth) * (3.14159265359 / srcHeight) * sinTheta;

        float mip = clamp (0.5 * log2 (omegaS / omegaP) + 1.0, 0.0, srcMips - 1.0);
        sum += g_envSource.SampleLevel (g_envSource_sampler, EnvUv (l), mip).rgb * ndotl;
        weight += ndotl;
    }

    // ⚠️ A ZERO WEIGHT IS REACHABLE AND MUST NOT DIVIDE. At the very smoothest
    // mip the lobe can be narrower than the sample set resolves; falling back
    // to the unfiltered direction is right there, not black.
    if (weight > 0.0)
        color = float4 (sum / weight, 1.0);
    else
        color = float4 (g_envSource.SampleLevel (g_envSource_sampler, EnvUv (n), 0.0).rgb, 1.0);
}
)hlsl";

constexpr const char* kArchVizEnvBackgroundPS = R"hlsl(
void main (float4 position : SV_POSITION, float3 ray : TEXCOORD0, out float4 color : SV_TARGET)
{
    // ⚠️ MIP 0, UNLIKE THE REFLECTION. The reflection blurs by roughness; the
    // background is what the eye compares the building against, and a blurred
    // sky behind a sharp model reads as the model being pasted on.
    float3 sky = g_envMap.SampleLevel (g_envMap_sampler, EnvUv (normalize (ray)), 0.0).rgb * g_envParams.x;

    // ⚠️ THE SAME ACES CURVE AND THE SAME 0.6 PRE-SCALE AS kArchVizMeshPSMain,
    // and they are not optional here. The model is tone mapped in Realistic
    // quality; an untouched HDR behind it clips to white wherever the sky is
    // brighter than 1.0, and the building then sits against a flat cut-out
    // instead of a sky. If that curve changes, this changes with it.
    // ⚠️ g_gradeParams.x, NOT A HARD-CODED CONSTANT. This used to be a literal
    // 0.6 while the model used the exposure control, so moving that control
    // brightened the building and left the sky where it was -- the two drifted
    // apart exactly when someone was trying to judge them together.
    //
    // ⚠️ CONDITIONAL ON g_frameControl.x, same as the mesh PS: HDR output skips
    // Grade here and the resolve pass applies it once to the whole frame.
    if (g_frameControl.x > 0.5)
        color = float4 (sky, 1.0);
    else
        color = float4 (Grade (sky), 1.0);
}
)hlsl";

constexpr const char* kArchVizGBufferDebugPS = R"hlsl(
Texture2D<float4> g_gbufferNormal;
Texture2D<float>  g_gbufferDepth;
Texture2D<float4> g_gbufferAlbedo;
Texture2D<float>  g_gbufferRoughness;
Texture2D<float4> g_gbufferMaterialData;
Texture2D<float2> g_gbufferMotion;
Buffer<uint>      g_depthRange;

// Depth-buffer value back to metres from the eye. ⚠️ THE TWO PROJECTIONS NEED
// DIFFERENT ARITHMETIC AND LOOK PLAUSIBLE WITH THE WRONG ONE: perspective depth
// is hyperbolic, parallel depth is linear, and using the linear form on a
// perspective buffer simply pushes everything to the far plane.
float LinearizeDepth (float depth, float nearClip, float farClip, bool perspective)
{
    if (perspective)
        return nearClip * farClip / max (farClip - depth * (farClip - nearClip), 1e-6);
    return lerp (nearClip, farClip, depth);
}

void main (float4 position : SV_POSITION, out float4 color : SV_TARGET)
{
    int2 pixel = int2 (position.xy);
    float depth = g_gbufferDepth.Load (int3 (pixel, 0));

    // Preserve the viewport's existing clear, including zero alpha on overlays.
    if (depth >= 1.0)
        discard;

    if (int (g_eyePos.w) == 7)
    {
        float3 normal = normalize (g_gbufferNormal.Load (int3 (pixel, 0)).xyz);
        color = float4 (normal * 0.5 + 0.5, 1.0);
    }
    else if (int (g_eyePos.w) == 10)
    {
        color = float4 (g_gbufferAlbedo.Load (int3 (pixel, 0)).rgb, 1.0);
    }
    else if (int (g_eyePos.w) == 11)
    {
        float roughness = saturate (g_gbufferRoughness.Load (int3 (pixel, 0)));
        color = float4 (roughness.xxx, 1.0);
    }
    else if (int (g_eyePos.w) == 12)
    {
        float4 materialData = g_gbufferMaterialData.Load (int3 (pixel, 0));
        // RGB shows roughness, classifier metalness and specular reflectance;
        // opacity remains in the target's alpha lane for downstream consumers.
        color = float4 (materialData.xyz, 1.0);
    }
    else if (int (g_eyePos.w) == 13)
    {
        // ---- RE51.C2: the motion vectors ----------------------------------
        //
        // ⚠️ RED IS RIGHTWARD MOTION, GREEN IS *UPWARD*, AND THE SCALE IS
        // ENORMOUS ON PURPOSE. A vector is stored in NDC, where a fast orbit
        // moves a pixel a few hundredths of a unit; shown at 1:1 the whole
        // screen is mid-grey whether the vectors are right, zero, or reversed.
        // x40 puts an ordinary drag into the visible range.
        //
        // ⚠️ WHAT A CORRECT FRAME LOOKS LIKE, so this can be judged rather than
        // admired: STILL CAMERA -> flat mid-grey (0.5, 0.5) everywhere, exactly.
        // Any colour at rest means the previous matrix is not last frame's.
        // PANNING RIGHT -> the model turns GREENISH-CYAN (content moves LEFT on
        // screen, so currNDC - prevNDC is negative in x). ORBITING -> a smooth
        // gradient across the model, not a flat wash: parts nearer the pivot
        // move less. THE SKY STAYS EXACTLY MID-GREY, because no triangle covers
        // it and the target is cleared -- a coloured sky means the clear was
        // lost, which is the one failure that would silently reproject the
        // background from wherever the building used to be.
        float2 motion = g_gbufferMotion.Load (int3 (pixel, 0));
        color = float4 (motion * 40.0 + 0.5, 0.5, 1.0);
    }
    else
    {
        float nearClip = g_shadowParams.x;
        float farClip = g_shadowParams.y;
        bool perspective = g_shadowParams.w > 0.5;

        // ⚠️ THE RAMP FITS THE FRAME, AND NEITHER FIXED ALTERNATIVE WORKS.
        // Anchored to the FRUSTUM (0.05 m .. 20 km) a whole building occupies a
        // few percent of the range and reads as uniform grey. Anchored to the
        // camera's ORBIT DISTANCE it clips to flat black the moment the model
        // sits outside that band. Both shipped, both were reported as a broken
        // depth buffer. g_depthRange carries the nearest and farthest depth
        // really on screen, reduced this frame -- see ArchViz/DiligentDepthRange.
        uint nearestBits = g_depthRange.Load (0);
        uint farthestBits = g_depthRange.Load (1);
        // Seeded min > max, so this is "the reduction wrote nothing" -- an empty
        // frame, or a view that never dispatched it. Fall back to the frustum
        // rather than to arithmetic on 0xFFFFFFFF read as a float (a NaN, which
        // would paint the whole image one undefined shade).
        float nearestDepth = nearestBits <= farthestBits ? asfloat (nearestBits) : 0.0;
        float farthestDepth = nearestBits <= farthestBits ? asfloat (farthestBits) : 1.0;

        float linearDistance = LinearizeDepth (depth, nearClip, farClip, perspective);
        float nearestLinear = max (LinearizeDepth (nearestDepth, nearClip, farClip, perspective), nearClip);
        float farthestLinear = LinearizeDepth (farthestDepth, nearClip, farClip, perspective);

        // ⚠️ THE SPAN IS CAPPED AT 1000:1, AND THAT IS WHAT STOPS THE GRADIENT
        // FLIPPING. Orbit far enough and a corner of the building passes close
        // to the eye; the true nearest depth then drops to centimetres while the
        // farthest stays at tens of metres, the log span triples, and every
        // surface that mattered is squeezed into the top of the ramp -- which
        // reads as the gradient inverting at particular camera angles. Pulling
        // the near end up instead sacrifices only the few pixels that are
        // practically touching the lens.
        nearestLinear = max (nearestLinear, farthestLinear / 1000.0);

        // Logarithmic across that measured span rather than linear: a frame
        // holding both a doorway and a horizon is a depth RATIO in the
        // thousands, and linear over it hands the whole near half one shade.
        // The floor on the span keeps a single-depth frame (one flat wall
        // filling the view) from dividing by ~0.
        float logSpan = max (log2 (farthestLinear / nearestLinear), 1e-3);
        float visibleDepth = 1.0 - saturate (log2 (max (linearDistance, nearestLinear) / nearestLinear) / logSpan);
        color = float4 (visibleDepth.xxx, 1.0);
    }
}
)hlsl";

constexpr const char* kArchVizAmbientOcclusionDebugPS = R"hlsl(
Texture2D<float> g_ambientOcclusion;
Texture2D<float> g_gbufferDepth;

void main (float4 position : SV_POSITION, out float4 color : SV_TARGET)
{
    int2 pixel = int2 (position.xy);
    if (g_gbufferDepth.Load (int3 (pixel, 0)) >= 1.0)
        discard;

    float ambientOcclusion = g_ambientOcclusion.Load (int3 (pixel, 0));
    color = float4 (ambientOcclusion.xxx, 1.0);
}
)hlsl";

// ---- the HDR resolve pass: tone maps the offscreen scene colour in ONE place --
//
// ⚠️ THIS IS THE PASS THAT REPLACES INLINE GRADE() IN THE MESH AND SKY SHADERS.
// When g_frameControl.x is 1, those shaders write raw scene-referred radiance
// (and premultiplied alpha) into the RGBA16_FLOAT HDR target; this full-screen
// pass reads it back, applies Grade() once, and writes display-referred colour
// into the swap chain's sRGB view.
//
// ⚠️ DISCARD ON ALPHA == 0, NOT CLEAR-COLOUR REPLACEMENT. The HDR target is
// cleared to transparent black; pixels no pass wrote stay at (0,0,0,0), and
// discarding them lets the swap chain's own clear (grey for the palette,
// transparent for the overlay) survive. That is what makes the overlay path
// work without a separate resolve: where the model isn't, the overlay shows
// Archicad's own 3D window.
//
// ⚠️ LOAD, NOT SAMPLE. The HDR target is exactly the viewport's own resolution,
// so pixel (x,y) maps 1:1 to texel (x,y). Filtering would only blur edges that
// the resolve has no reason to blur.
// ---- RE51.C8: coverage, lifted out of the HDR alpha so TAA can resolve it ----
//
// ⚠️ THIS PASS EXISTS BECAUSE DILIGENTFX'S TAA READS Texture2D<float3> AND
// WRITES ITS OWN HISTORY WEIGHT INTO ALPHA. Coverage cannot ride through it in
// the channel it already lives in, so it is broadcast into RGB here and run
// through a SECOND accumulation buffer (index 1) with the same motion vectors,
// depth and cameras as the colour. What comes back is coverage that has been
// reprojected and disocclusion-rejected exactly like the radiance beside it.
//
// ⚠️ WHY IT MATTERS AT ALL, given that the resolve only tests `> 0`. Coverage
// is BINARY per pixel and the projection is JITTERED, so a silhouette pixel
// flips covered/uncovered every frame and the resolve's discard flips with it.
// That is not aliasing TAA can fix from the colour side -- the pixel is either
// in the image or it is not -- and it reads as the whole edge crawling. It was
// reported live on 2026-08-24 as "jitters quite a lot and is very distracting",
// with TAA confirmed resolving, which is exactly the signature: the interior is
// steady and the edges are not.
constexpr const char* kArchVizCoveragePS = R"hlsl(
Texture2D<float4> g_hdrColor;

void main (float4 position : SV_POSITION, out float4 color : SV_TARGET)
{
    // ⚠️ BROADCAST TO ALL FOUR CHANNELS, not just red. TAA overwrites alpha
    // with its history weight, so alpha here is discarded -- but the copy TAA
    // makes on a reset frame keeps whatever it was handed, and a coverage that
    // reads back as zero on those frames would blink the whole image out.
    float coverage = g_hdrColor.Load (int3 (int2 (position.xy), 0)).a;
    color = float4 (coverage, coverage, coverage, coverage);
}
)hlsl";

// ---- RE51.C7/C8: SSR composited into the HDR radiance, BEFORE TAA ----------
//
// ⚠️ THE ORDER IS THE WHOLE POINT OF THIS PASS EXISTING. This code used to be a
// branch inside the resolve, which runs AFTER temporal anti-aliasing -- so the
// reflections were the one part of the image TAA could not touch, and they
// jittered at every stability setting while everything around them was steady.
// Tutorial27_PostProcessing composites SSR into the radiance in ComputeLighting
// and only then calls ComputeTAA; this pass restores that order.
//
// ⚠️ IT WRITES HDR, NOT DISPLAY-REFERRED COLOUR. No Grade() here -- the resolve
// still owns tone mapping, and applying it twice would crush the image. Alpha
// is carried through untouched because it is the geometric coverage the resolve
// and the coverage accumulation both depend on.
constexpr const char* kArchVizSsrCompositePS = R"hlsl(
Texture2D<float4> g_hdrColor;
Texture2D<float4> g_ssrColor;
Texture2D<float>  g_gbufferRoughness;
Texture2D<float4> g_gbufferNormal;
Texture2D<float4> g_gbufferAlbedo;
Texture2D<float4> g_gbufferMaterialData;

void main (float4 position : SV_POSITION, out float4 color : SV_TARGET)
{
    int2 pixel = int2 (position.xy);
    float4 hdr = g_hdrColor.Load (int3 (pixel, 0));
    float3 radiance = hdr.rgb;

    // Match Tutorial27_PostProcessing's ComputeSpecularIBL: interpolate between
    // the prefiltered environment radiance and SSR first, then apply the
    // split-sum BRDF. The HDR pixel already contains the environment term, so
    // replace that term only. Replacing the entire HDR pixel also replaces its
    // direct, diffuse and transmitted light and turns a reflection into an
    // opaque duplicate of the reflected object.
    //
    // ⚠️ GATED BY ROUGHNESS AND SSR ALPHA. A rough surface gets no SSR because
    // its reflection is a diffuse blur that screen-space rays cannot represent.
    // The SSR's own alpha is its confidence; where it is zero, the HDR colour
    // survives unchanged.
    //
    // ⚠️ g_gradeParams.w IS THE SSR INTENSITY IN THIS PASS. In the mesh shader
    // the same lane is the AO intensity; AO is already baked into the HDR colour
    // by the time this runs, so the lane is free. Draw uploads it before the
    // composite draw and again before the resolve.
    float roughness = g_gbufferRoughness.Load (int3 (pixel, 0));
    if (roughness < 1.0 && g_gradeParams.w > 0.0)
    {
        float4 ssr = g_ssrColor.Load (int3 (pixel, 0));
        float ssrWeight = saturate (ssr.a * saturate (1.0 - roughness) * g_gradeParams.w);

        uint width, height;
        g_hdrColor.GetDimensions (width, height);
        float2 ndc = float2 ((position.x / float (width)) * 2.0 - 1.0,
                             1.0 - (position.y / float (height)) * 2.0);
        float3 viewRay = normalize (g_envRayForward.xyz + ndc.x * g_envRayRight.xyz +
                                    ndc.y * g_envRayUp.xyz);
        float3 viewDir = -viewRay;
        float3 normal = normalize (g_gbufferNormal.Load (int3 (pixel, 0)).xyz);
        float4 material = g_gbufferMaterialData.Load (int3 (pixel, 0));
        float3 baseColor = g_gbufferAlbedo.Load (int3 (pixel, 0)).rgb;
        float metallic = saturate (material.y);
        float f0scalar = 0.08 * saturate (material.z);
        float3 f0 = lerp (float3 (f0scalar, f0scalar, f0scalar), baseColor, metallic);
        float ndotv = saturate (dot (normal, viewDir));

        // Same analytic split-sum fit as the forward shader's environment term.
        float4 c0 = float4 (-1.0, -0.0275, -0.572, 0.022);
        float4 c1 = float4 ( 1.0,  0.0425,  1.040, -0.040);
        float4 rp = roughness * c0 + c1;
        float a004 = min (rp.x * rp.x, exp2 (-9.28 * ndotv)) * rp.x + rp.y;
        float2 envAB = float2 (-1.04, 1.04) * a004 + rp.zw;
        float3 envBrdf = f0 * envAB.x + envAB.y;

        float3 reflected = reflect (-viewDir, normal);
        float3 envColor;
        if (g_envParams.z > 0.5)
        {
            float mip = saturate (roughness) * max (g_envParams.w - 1.0, 0.0);
            envColor = g_envMap.SampleLevel (g_envMap_sampler, EnvUv (reflected), mip).rgb * g_envParams.x;
        }
        else
        {
            envColor = lerp (g_groundColor.rgb, g_skyColor.rgb,
                             saturate (reflected.z * 0.5 + 0.5));
        }

        radiance += (ssr.rgb - envColor) * envBrdf * g_gradeParams.y * g_skyColor.w * ssrWeight;
    }

    color = float4 (radiance, hdr.a);
}
)hlsl";

constexpr const char* kArchVizResolvePS = R"hlsl(
Texture2D<float4> g_hdrColor;
Texture2D<float4> g_hdrCoverage;

void main (float4 position : SV_POSITION, out float4 color : SV_TARGET)
{
    int2 pixel = int2 (position.xy);
    float4 hdr = g_hdrColor.Load (int3 (pixel, 0));
    float4 coverageTexel = g_hdrCoverage.Load (int3 (pixel, 0));

    // ⚠️ THE CHANNEL DEPENDS ON WHICH TEXTURE IS BOUND, AND g_frameControl.y
    // SAYS WHICH. DiligentFX TAA stores its history weight in output alpha, so
    // TAA-resolved coverage cannot live there: kArchVizCoveragePS broadcasts it
    // into RGB before the accumulation and it comes back in RED. When TAA is
    // off there is no jitter to resolve, nothing runs that pass, and this frame's
    // own HDR target is bound with coverage still in ALPHA.
    float coverage = g_frameControl.y > 0.5 ? coverageTexel.r : coverageTexel.a;

    // ⚠️ A THRESHOLD, NOT `> 0`, ON THE RESOLVED PATH. Accumulated coverage is
    // FRACTIONAL at a silhouette -- that is the whole point of resolving it --
    // so testing against zero would keep every pixel the edge has touched in the
    // last dozen frames and fatten the model by a pixel. Half a pixel of
    // coverage is the same rule the rasteriser itself uses.
    if (coverage <= (g_frameControl.y > 0.5 ? 0.5 : 0.0))
        discard;

    float3 radiance = hdr.rgb;

    // ⚠️ THE SSR COMPOSITION USED TO LIVE HERE AND DELIBERATELY NO LONGER DOES.
    // It ran AFTER temporal anti-aliasing, so nothing could stabilise it: the
    // reflections jittered at every stability setting while the rest of the
    // image was steady, reported live on 2026-08-24. Tutorial27_PostProcessing
    // composites SSR into the radiance BEFORE ComputeTAA for exactly this
    // reason. kArchVizSsrCompositePS is that pass; this one now only tone-maps.
    color = float4 (Grade (radiance), coverage);
}
)hlsl";

// The environment texture and its lookup, SHARED by the mesh shading and the
// sky background. ⚠️ ONE COPY BY CONSTRUCTION. EnvUv is an ABI with
// ArchViz/EnvironmentLighting's `DirectionAt` (the offline test
// `DirectionConventionIsPinned` holds the other end), so a second transcription
// of it in the background shader would be a convention free to drift silently:
// the model would be lit by one sky and sit against another, which on a
// photographic environment is genuinely hard to see and impossible to explain.
constexpr const char* kArchVizEnvCommonPS = R"hlsl(
Texture2D    g_envMap;
SamplerState g_envMap_sampler;

// The equirectangular lookup.
//
// ⚠️ THIS IS AN ABI WITH ArchViz/EnvironmentLighting's `DirectionAt`, AND THE
// OFFLINE TEST `DirectionConventionIsPinned` EXISTS TO HOLD THE OTHER END OF IT.
// The SH coefficients are integrated on the CPU against that convention; if this
// disagrees, the diffuse light and the reflection come from different skies --
// the building is lit from one direction and reflects another, which on a
// photographic environment is genuinely hard to see and impossible to explain.
//
//   v = 0 is the +Z pole, v = 1 is -Z   (row 0 is up, Archicad is Z-up)
//   u = 0 is phi = 0, i.e. +X, advancing counter-clockwise toward +Y
float2 EnvUv (float3 dir)
{
    float3 d = normalize (dir);
    // ⚠️ CLAMPED BEFORE acos. A normalise can land a hair outside [-1,1] and
    // acos of that is NaN, which paints one black pixel at each pole -- and it
    // moves as the camera moves, so it reads as a flickering artefact rather
    // than as a domain error.
    float theta = acos (clamp (d.z, -1.0, 1.0));
    float phi = atan2 (d.y, d.x) + g_envParams.y;
    // atan2 returns -pi..pi and the rotation can push it anywhere; wrap into
    // 0..2pi rather than relying on the sampler's address mode, because v must
    // stay CLAMPed even while u WRAPs.
    const float twoPi = 6.28318530718;
    phi = phi - twoPi * floor (phi / twoPi);
    return float2 (phi / twoPi, theta / 3.14159265359);
}

// The EXACT INVERSE of EnvUv, and it is here rather than in the prefilter
// shader for the reason EnvUv itself is here: two transcriptions of one
// convention drift, and the drift is invisible.
//
// ⚠️ THE PREFILTER IS THE ONLY CALLER, and it runs with g_envParams.y == 0, so
// the rotation term below is the identity there. It is written out anyway
// because a version that silently ignored the rotation would be a DIFFERENT
// function from EnvUv, which is precisely what this pairing exists to prevent.
float3 EnvDir (float2 uv)
{
    const float twoPi = 6.28318530718;
    float phi = uv.x * twoPi - g_envParams.y;
    float theta = uv.y * 3.14159265359;
    float sinTheta = sin (theta);
    return float3 (sinTheta * cos (phi), sinTheta * sin (phi), cos (theta));
}

// ---- ACES, the FITTED form, replacing Narkowicz's per-channel curve ---------
//
// ⚠️ THE OLD CURVE WAS APPLIED TO EACH CHANNEL INDEPENDENTLY, AND THAT IS WHAT
// OVER-SATURATED THE IMAGE. A per-channel curve compresses a bright channel more
// than a dark one, so on an already-saturated surface it pushes the channels
// FURTHER apart. Measured on this project's own grass, authored sRGB
// (0.34, 0.66, 0.18) under unit white light: the per-channel curve rendered it
// (0.26, 0.63, 0.08) -- the blue channel more than halved -- while the fit below
// renders (0.30, 0.62, 0.14), close to what was authored.
//
// This is Stephen Hill's fit to the real ACES RRT+ODT: into the ACES working
// space, through the tone curve, and back. The matrices are what make it
// desaturate HIGHLIGHTS the way film does instead of saturating MIDTONES the way
// a per-channel curve does.
//
// ⚠️ THE GREY RESPONSE IS DELIBERATELY UNCHANGED. Narkowicz at the old 0.6
// pre-scale and this fit at 1.2 agree on neutral grey to within 0.002 across the
// range (0.10 -> 0.024 vs 0.019, 0.50 -> 0.458 vs 0.454, 1.00 -> 0.840 vs
// 0.841), which is why the default exposure moved with the curve. Brightness is
// held; only colour rendition changes. Do not move one without the other.
//
// ⚠️ STILL BEFORE THE HARDWARE sRGB ENCODE, NOT INSTEAD OF IT. The render target
// is an _SRGB view; adding a pow(1/2.2) here gamma-corrects twice.
float3 AcesFitted (float3 colour)
{
    // Row-major, and mul(M, v) treats v as a column vector -- the orientation
    // Hill's original HLSL uses. Transposing either matrix silently produces a
    // plausible-looking image with the wrong primaries.
    const float3x3 acesIn = float3x3 (0.59719, 0.35458, 0.04823,
                                      0.07600, 0.90834, 0.01566,
                                      0.02840, 0.13383, 0.83777);
    const float3x3 acesOut = float3x3 ( 1.60475, -0.53108, -0.07367,
                                       -0.10208,  1.10813, -0.00605,
                                       -0.00327, -0.07276,  1.07602);
    float3 v = mul (acesIn, colour);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return saturate (mul (acesOut, a / b));
}

// ---- the ONE finishing chain, shared by the model and the sky ---------------
//
// ⚠️ IT IS A FUNCTION BECAUSE THE TWO CALL SITES ALREADY DRIFTED ONCE. The sky
// background carried a hard-coded 0.6 pre-scale while the model read
// g_gradeParams.x, so moving the exposure control brightened the building and
// left the sky exactly where it was -- and the whole reason a sky is drawn at
// all is to have something to judge the building against. White balance is
// added here rather than at either call site for the same reason.
//
// ORDER MATTERS AND IT IS THE PHOTOGRAPHIC ONE: white balance is a property of
// the LIGHT, so it is applied to scene-referred radiance; exposure scales that
// radiance; the tone curve then maps it to display. Balancing AFTER the curve
// would tint the clipped highlights, which is the artefact that reads as a
// cheap filter.
//
// ⚠️ STILL BEFORE THE HARDWARE sRGB ENCODE. The render target is an _SRGB view.
float3 Grade (float3 radiance)
{
    return AcesFitted (radiance * g_whiteBalance.rgb * g_gradeParams.x);
}

)hlsl";

// One mesh pixel shader per DiligentFX representation. ShadowMapManager changes
// the atlas format with the mode, and Shadows.fxh deliberately resolves that
// texture type at compile time.
constexpr const char* kArchVizShadowModePcf = R"hlsl(
#define SHADOW_MODE 1
#ifndef _HLSL_DEFINITIONS_
#define _HLSL_DEFINITIONS_
#define NDC_MIN_Z 0.0
#define F3NDC_XYZ_TO_UVD_SCALE float3(0.5, -0.5, 1.0)
float2 NormalizedDeviceXYToTexUV (float2 p) { return float2(0.5, 0.5) + float2(0.5, -0.5) * p; }
float NormalizedDeviceZToDepth (float z) { return z; }
#endif
#define PCF_FILTER_SIZE 0
#define FILTER_ACROSS_CASCADES 1
#define BEST_CASCADE_SEARCH 1
#include "BasicStructures.fxh"
#include "Shadows.fxh"
)hlsl";

constexpr const char* kArchVizShadowModeVsm = R"hlsl(
#define SHADOW_MODE 2
#ifndef _HLSL_DEFINITIONS_
#define _HLSL_DEFINITIONS_
#define NDC_MIN_Z 0.0
#define F3NDC_XYZ_TO_UVD_SCALE float3(0.5, -0.5, 1.0)
float2 NormalizedDeviceXYToTexUV (float2 p) { return float2(0.5, 0.5) + float2(0.5, -0.5) * p; }
float NormalizedDeviceZToDepth (float z) { return z; }
#endif
#define FILTER_ACROSS_CASCADES 1
#define BEST_CASCADE_SEARCH 1
#include "BasicStructures.fxh"
#include "Shadows.fxh"
)hlsl";

constexpr const char* kArchVizShadowModeEvsm2 = R"hlsl(
#define SHADOW_MODE 3
#ifndef _HLSL_DEFINITIONS_
#define _HLSL_DEFINITIONS_
#define NDC_MIN_Z 0.0
#define F3NDC_XYZ_TO_UVD_SCALE float3(0.5, -0.5, 1.0)
float2 NormalizedDeviceXYToTexUV (float2 p) { return float2(0.5, 0.5) + float2(0.5, -0.5) * p; }
float NormalizedDeviceZToDepth (float z) { return z; }
#endif
#define FILTER_ACROSS_CASCADES 1
#define BEST_CASCADE_SEARCH 1
#include "BasicStructures.fxh"
#include "Shadows.fxh"
)hlsl";

constexpr const char* kArchVizShadowModeEvsm4 = R"hlsl(
#define SHADOW_MODE 4
#ifndef _HLSL_DEFINITIONS_
#define _HLSL_DEFINITIONS_
#define NDC_MIN_Z 0.0
#define F3NDC_XYZ_TO_UVD_SCALE float3(0.5, -0.5, 1.0)
float2 NormalizedDeviceXYToTexUV (float2 p) { return float2(0.5, 0.5) + float2(0.5, -0.5) * p; }
float NormalizedDeviceZToDepth (float z) { return z; }
#endif
#define FILTER_ACROSS_CASCADES 1
#define BEST_CASCADE_SEARCH 1
#include "BasicStructures.fxh"
#include "Shadows.fxh"
)hlsl";

constexpr const char* kArchVizMeshPS = R"hlsl(
// ShadowMapAttribs and the sampling functions are DiligentFX's own definitions.
// kArchVizShadowMode* is prepended before this literal and selects the matching
// resource representation at shader compile time.
cbuffer ArchVizShadowConstants
{
    ShadowMapAttribs g_shadowAttribs;
    // x = enabled; y = tan(light angular radius); z = blocker-search metres;
    // w = maximum penumbra radius in metres.
    float4 g_pcssParams;
};

// ---- RE51.C5a: how far along the normal a receiver is pushed ---------------
//
// ⚠️ IN TEXELS, AND THE TEXEL IS THE ONE BELONGING TO THE CASCADE THAT WILL
// ACTUALLY BE SAMPLED. See SampleSunShadow: this used to be multiplied by
// cascade ZERO's texel size for every cascade, which is correct only for the
// nearest one. With four cascades at a 0.95 partitioning factor the outermost
// texel is well over an order of magnitude larger, so distant receivers were
// offset by a small fraction of the distance they needed and leaned entirely on
// the global fFixedDepthBias to hide the acne -- which is a flat depth offset,
// so buying enough of it to clean the far cascades detaches contact shadows in
// the near one. Sizing the offset per cascade is what lets that bias stay small.
//
// 2.0 reproduces the previous near-field behaviour exactly, because the old
// expression was cascade-0 texel metres times two.
static const float kNormalOffsetTexels = 2.0;

#if SHADOW_MODE == SHADOW_MODE_PCF
Texture2DArray<float>  g_shadowMap;
SamplerComparisonState g_shadowMap_sampler;
#else
Texture2DArray<float4> g_filterableShadowMap;
SamplerState           g_filterableShadowMap_sampler;
#endif

// ---- RE51.C3: the frame's ambient occlusion --------------------------------
//
// ⚠️ NO SAMPLER, AND `Load` RATHER THAN `Sample`. The AO texture is exactly the
// size of the surface being drawn, so there is a one-to-one mapping from pixel
// to texel and any filtering would only blur an already-blurred quantity. Load
// also takes integer coordinates, which removes the half-texel question
// entirely -- and a half-texel slip in an occlusion term looks like a rim of
// light around every contact edge, which reads as a lighting choice.
//
// ⚠️ IT IS DECLARED WHETHER OR NOT ONE IS BOUND, and the gate is
// `g_gradeParams.w`, NOT a null check. HLSL cannot ask whether a texture is
// bound; an unbound Load returns ZERO, and zero occlusion means fully occluded,
// which would render the whole model black.
Texture2D<float> g_ambientOcclusion;

// The sky's diffuse contribution for a world normal, reconstructed from the nine
// coefficients. The irradiance convolution and the 1/pi are ALREADY FOLDED INTO
// the coefficients on the CPU, so this is a plain dot product with the basis and
// the result multiplies albedo directly.
float3 ShDiffuse (float3 n)
{
    float3 result = g_sh[0].rgb * 0.282095
                  + g_sh[1].rgb * (0.488603 * n.y)
                  + g_sh[2].rgb * (0.488603 * n.z)
                  + g_sh[3].rgb * (0.488603 * n.x)
                  + g_sh[4].rgb * (1.092548 * n.x * n.y)
                  + g_sh[5].rgb * (1.092548 * n.y * n.z)
                  + g_sh[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0))
                  + g_sh[7].rgb * (1.092548 * n.x * n.z)
                  + g_sh[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    // ⚠️ CLAMPED, for the reason EvaluateDiffuse is clamped on the CPU: order-2
    // SH rings around a small bright sun and really does reconstruct negative,
    // which here would SUBTRACT light from the surface.
    return max (result, 0.0.xxx);
}

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal   : NORMAL;
    float4 color    : COLOR0;
};

struct PSOutput
{
    float4 color : SV_TARGET;
};

#if SHADOW_MODE == SHADOW_MODE_PCF
// ---- RE51.C6a: the sampling floor, the dither and the disk ------------------
//
// The three constants below are why low-resolution shadows stopped being jagged.
// They are stated here rather than buried in the filter because each one has a
// counterpart on the CPU or in DiligentFX that must not drift from it.

// ⚠️ THE MINIMUM FILTER RADIUS IS IN TEXELS, NOT METRES, AND THAT IS THE WHOLE
// POINT. The penumbra this shader computes is a physical quantity: at a contact
// edge it is genuinely near zero, and the previous code honoured that by
// collapsing to ONE hardware comparison sample. One 2x2 comparison tap is a
// staircase -- so contact hardening, which exists to make that edge crisp, was
// what made it jagged, and worse the lower the resolution.
//
// A shadow map cannot resolve detail below its own texel either way, so
// filtering across a couple of texels there throws away NO real information; it
// only replaces a hard staircase with the gradient the map can actually
// represent. Because the floor is expressed in texels it tightens automatically
// as resolution rises: at 4096 it is a fraction of the penumbra and invisible,
// at 512 it is what stops the edge aliasing.
//
// ⚠️ KEEP IN STEP WITH kMinFilterTexels IN DiligentShadowMap.cpp, which applies
// the same floor to fFilterWorldSize for the non-PCSS PCF path. The two paths
// looking different at the same resolution is a bug, not a mode.
static const float kPcssMinFilterTexels = 2.0;

// Below this radius the wide filter buys nothing that eight taps do not already
// resolve, so the tap count drops with it. Contact edges are the common case and
// are now the CHEAP case -- the old code spent 16 taps on wide penumbrae and 1
// on contacts, which is backwards for both quality and cost.
static const float kPcssNarrowFilterTexels = 3.0;
static const int   kPcssNarrowTaps = 8;
static const int   kPcssWideTaps = 16;
static const int   kPcssBlockerTaps = 12;

// ⚠️ THE DITHER IS KEYED ON THE SCREEN PIXEL, NOT ON THE SHADOW-MAP UV. The
// previous hash read `uv * f4ShadowMapDim.xy`, which is constant within a shadow
// texel -- so at 512 every screen pixel covered by one texel drew the SAME
// rotation and the dither correlated into blocks instead of dispersing. That is
// the second half of the jaggedness, and it got worse exactly where the map got
// coarser.
//
// ⚠️ AND IT ADVANCES WITH THE FRAME, WHICH IS WHAT MAKES THE TAP BUDGET GO
// FURTHER THAN IT LOOKS. g_shadowParams.y carries a frame phase when TAA is
// running and ZERO when it is not (see DiligentSceneDraw). With TAA at 0.9 the
// history averages roughly ten frames of DIFFERENT rotations, so 16 taps resolve
// like far more; without it the term vanishes and the dither is merely spatial,
// which is still strictly better than the block-correlated hash it replaces.
// Cycling the phase with no temporal filter downstream would be visible crawl,
// so the gate belongs on the CPU where TAA's real state is known.
//
// Interleaved gradient noise rather than frac(sin(dot(...))): the same cost, a
// far more even distribution over a pixel neighbourhood, and no dependence on
// sin() precision at the large coordinates a 2048-texel map produces.
float PcssDitherPhase (float2 screenPixel)
{
    float2 p = screenPixel + 5.588238 * g_shadowParams.y;
    return frac (52.9829189 * frac (dot (p, float2 (0.06711056, 0.00583715)))) * 6.2831853;
}

// A Vogel (golden-angle) disk, generated rather than tabulated. It is uniform at
// EVERY tap count, which is what lets the count vary with the penumbra above --
// a fixed 16-entry table sampled 8 times is not a disk, it is half a disk. The
// per-pixel phase is the rotation, so no separate rotate step is needed.
float2 VogelDisk (int index, int count, float phase)
{
    float radius = sqrt ((float (index) + 0.5) / float (count));
    float theta = float (index) * 2.39996323 + phase;
    float sine;
    float cosine;
    sincos (theta, sine, cosine);
    return float2 (cosine, sine) * radius;
}

float FilterPcssCascade (float3 ddxLightPosition, float3 ddyLightPosition, CascadeSamplingInfo sampling,
                         float2 screenPixel)
{
    float3 ddxShadow = ddxLightPosition * sampling.f3LightSpaceScale * F3NDC_XYZ_TO_UVD_SCALE;
    float3 ddyShadow = ddyLightPosition * sampling.f3LightSpaceScale * F3NDC_XYZ_TO_UVD_SCALE;
    float2 receiverBias = ComputeReceiverPlaneDepthBias (ddxShadow, ddyShadow);
    float2 biasClamp = abs ((sampling.f3LightSpaceScale.z * F3NDC_XYZ_TO_UVD_SCALE.z) /
                            (sampling.f3LightSpaceScale.xy * F3NDC_XYZ_TO_UVD_SCALE.xy)) *
                       g_shadowAttribs.fReceiverPlaneDepthBiasClamp;
    receiverBias = clamp (receiverBias, -biasClamp, biasClamp) * g_shadowAttribs.f4ShadowMapDim.zw;
    float receiverDepth = sampling.fDepth - dot (1.0.xx, abs (receiverBias)) - g_shadowAttribs.fFixedDepthBias;

    // ⚠️ THE BLOCKER SEARCH AND THE FILTER USE THE SAME DITHER WITH A HALF-TURN
    // BETWEEN THEM. Sharing one phase outright correlates the estimate with the
    // samples taken from it, which biases the penumbra; the offset decorrelates
    // them for free.
    float phase = PcssDitherPhase (screenPixel);

    float2 searchRadius = g_pcssParams.z * abs (sampling.f3LightSpaceScale.xy) * 0.5;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int i = 0; i < kPcssBlockerTaps; ++i) {
        float2 offset = VogelDisk (i, kPcssBlockerTaps, phase + 3.14159265) * searchRadius;
        int2 texel = int2 ((sampling.f2UV + offset) * g_shadowAttribs.f4ShadowMapDim.xy);
        texel = clamp (texel, int2 (0, 0), int2 (g_shadowAttribs.f4ShadowMapDim.xy) - 1);
        float depth = g_shadowMap.Load (int4 (texel, sampling.iCascadeIdx, 0));
        if (depth < receiverDepth) {
            blockerDepth += depth;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
        return 1.0;

    float averageBlockerDepth = blockerDepth / blockerCount;
    float blockerSeparation = (receiverDepth - averageBlockerDepth) /
                              max (abs (sampling.f3LightSpaceScale.z), 1e-6);
    float penumbraMetres = min (blockerSeparation * g_pcssParams.y, g_pcssParams.w);
    float2 filterRadius = penumbraMetres * abs (sampling.f3LightSpaceScale.xy) * 0.5;

    // ⚠️ THE FLOOR IS APPLIED AS A max() IN UV, NOT AS A SCALE FACTOR. Dividing
    // to rescale a radius that can legitimately be zero at a contact edge
    // produces inf * 0 -- a NaN that survives the average and paints a black
    // pixel. See kPcssMinFilterTexels for why the floor exists at all.
    filterRadius = max (filterRadius, kPcssMinFilterTexels * g_shadowAttribs.f4ShadowMapDim.zw);
    float2 filterRadiusTexels = filterRadius * g_shadowAttribs.f4ShadowMapDim.xy;

    // The tap count follows the radius: eight taps cover a two-to-three texel
    // disk as evenly as sixteen do, and the Vogel sequence stays uniform at both.
    int tapCount = max (filterRadiusTexels.x, filterRadiusTexels.y) <= kPcssNarrowFilterTexels
                       ? kPcssNarrowTaps
                       : kPcssWideTaps;

    float lightAmount = 0.0;
    // ⚠️ [loop], NOT [unroll]. The trip count is a per-pixel value now, so an
    // unroll request against it either fails to compile or forces the maximum
    // and throws the saving away.
    [loop]
    for (int j = 0; j < tapCount; ++j) {
        float2 offset = VogelDisk (j, tapCount, phase) * filterRadius;
        float sampleDepth = receiverDepth + dot (offset * g_shadowAttribs.f4ShadowMapDim.xy, receiverBias);
        lightAmount += g_shadowMap.SampleCmpLevelZero (
            g_shadowMap_sampler, float3 (sampling.f2UV + offset, sampling.iCascadeIdx), sampleDepth);
    }
    return lightAmount / float (tapCount);
}

FilteredShadow FilterPcssShadowMap (float3 lightPosition, float3 ddxLightPosition,
                                    float3 ddyLightPosition, float cameraDepth, float2 screenPixel)
{
    CascadeSamplingInfo sampling = FindCascade (g_shadowAttribs, lightPosition, cameraDepth);
    FilteredShadow shadow;
    shadow.iCascadeIdx = sampling.iCascadeIdx;
    shadow.fNextCascadeBlendAmount = 0.0;
    shadow.fLightAmount = 1.0;
    if (sampling.iCascadeIdx == g_shadowAttribs.iNumCascades)
        return shadow;

    shadow.fLightAmount = FilterPcssCascade (ddxLightPosition, ddyLightPosition, sampling, screenPixel);
    if (sampling.iCascadeIdx + 1 < g_shadowAttribs.iNumCascades) {
        CascadeSamplingInfo nextSampling = GetCascadeSamplingInfo (
            g_shadowAttribs, lightPosition, sampling.iCascadeIdx + 1);
        shadow.fNextCascadeBlendAmount = GetNextCascadeBlendAmount (
            g_shadowAttribs, cameraDepth, sampling, nextSampling);
        if (shadow.fNextCascadeBlendAmount > 0.0) {
            float nextLightAmount =
                FilterPcssCascade (ddxLightPosition, ddyLightPosition, nextSampling, screenPixel);
            shadow.fLightAmount = lerp (shadow.fLightAmount, nextLightAmount,
                                        shadow.fNextCascadeBlendAmount);
        }
    }
    return shadow;
}
#endif

// x = light amount; yzw = DiligentFX's cascade debug colour.
float4 SampleSunShadow (float3 worldPos, float3 normal, float2 screenPixel)
{
    if (g_shadowParams.z < 0.5)
        return float4 (1.0, 1.0, 1.0, 1.0);

    float cameraDepth = mul (g_viewProj, float4 (worldPos, 1.0)).w;

    // ---- which cascade, before deciding how far to push off the surface -----
    //
    // ⚠️ THE CASCADE IS RESOLVED TWICE ON PURPOSE, AND THE ORDER IS THE POINT.
    // The offset has to be sized by the cascade's texel, but the cascade is
    // chosen from the position the offset produces -- so this probe finds the
    // cascade with the UNSHIFTED position first, and the real lookup below runs
    // with the shifted one. FindCascade is a short loop over at most eight
    // margin tests and no texture reads; paying for it twice is far cheaper
    // than the fFixedDepthBias increase that the alternative demands.
    //
    // ⚠️ AND THE OFFSET IS ZERO WHEN THE PROBE FALLS OUTSIDE EVERY CASCADE.
    // Scaling by a stale f3LightSpaceScale there could shift the point back
    // INTO a cascade and shadow a receiver that the shadow map never covered.
    // Zero leaves it outside, which is what the un-offset path already decided.
    float4 probePosition = mul (g_shadowAttribs.mWorldToLightView, float4 (worldPos, 1.0));
    CascadeSamplingInfo probe =
        FindCascade (g_shadowAttribs, probePosition.xyz / probePosition.w, cameraDepth);

    float offsetMetres = 0.0;
    if (probe.iCascadeIdx < g_shadowAttribs.iNumCascades) {
        // f3LightSpaceScale maps world units into NDC's [-1, 1], so the cascade
        // spans 2 / |scale.x| metres across f4ShadowMapDim.x texels. This is the
        // shader-side twin of FirstCascadeTexelMetresOf in DiligentShadowMap.cpp,
        // generalised to any cascade rather than only the first.
        float texelMetres =
            2.0 / max (abs (probe.f3LightSpaceScale.x) * g_shadowAttribs.f4ShadowMapDim.x, 1e-6);
        offsetMetres = texelMetres * kNormalOffsetTexels;
    }

    float3 offsetPos = worldPos + normal * offsetMetres;
    float4 lightPosition = mul (g_shadowAttribs.mWorldToLightView, float4 (offsetPos, 1.0));
    float3 lightViewPosition = lightPosition.xyz / lightPosition.w;

    FilteredShadow shadow;
#if SHADOW_MODE == SHADOW_MODE_PCF
    if (g_pcssParams.x > 0.5)
        shadow = FilterPcssShadowMap (lightViewPosition, ddx (lightViewPosition),
                                      ddy (lightViewPosition), cameraDepth, screenPixel);
    else
        shadow = FilterShadowMap (g_shadowAttribs, g_shadowMap, g_shadowMap_sampler,
                                  lightViewPosition, ddx (lightViewPosition), ddy (lightViewPosition),
                                  cameraDepth);
#else
    shadow = SampleFilterableShadowMap (g_shadowAttribs, g_filterableShadowMap,
                                        g_filterableShadowMap_sampler, lightViewPosition,
                                        ddx (lightViewPosition), ddy (lightViewPosition), cameraDepth);
#endif
    return float4 (shadow.fLightAmount, GetCascadeColor (shadow));
}

)hlsl";

// ...and the entry point.
//
// ⚠️ SPLIT FROM THE HELPERS ABOVE BY AN MSVC LIMIT, NOT BY DESIGN. A single
// string literal may not exceed 16 KB (C2026), and the mesh pixel shader
// passed it when the environment lighting arrived -- as a COMPILE error
// naming only the closing line of the literal, which is a long way from
// anything that looks like a cause. The pieces are concatenated at runtime by
// ArchVizShaderSource, the same mechanism the shared cbuffer already uses;
// splitting further needs no new machinery, just another argument.
constexpr const char* kArchVizMeshPSMain = R"hlsl(
void main (in PSInput psIn, out PSOutput psOut)
{
    float3 n = normalize (psIn.normal);

    // Two-sided, but NOT abs(dot(n, l)): that lights a north wall as brightly
    // as a south one and throws the sun's direction away. Flip the normal
    // toward the viewer instead. See fs_archviz_mesh.sc for the full account.
    float3 v = normalize (g_eyePos.xyz - psIn.worldPos);
    if (dot (n, v) < 0.0)
        n = -n;

    // ---- ambient: sky above, ground bounce below -------------------------
    // ⚠️ n.z, NOT n.y. Archicad is Z-UP, and this is the one place in the
    // shader where that matters: with n.y the ambient gradient would run
    // north-south and every roof would be lit like a north wall.
    float  hemi = saturate (n.z * 0.5 + 0.5);
    float3 ambientColor = lerp (g_groundColor.rgb, g_skyColor.rgb, hemi);
    float  ambient = g_sunAndAmbient.w;

    // ---- the HDR sky, when one is loaded ----------------------------------
    //
    // ⚠️ IT REPLACES THE TWO-COLOUR AMBIENT RATHER THAN ADDING TO IT. The
    // hemispheric lerp above is a STAND-IN for exactly this quantity -- the
    // light arriving from the sky and the ground -- so keeping both would count
    // the environment twice and light the model at roughly double the intensity
    // it was tuned for. `envEnabled` is the switch between an approximation and
    // the thing it approximates, never a sum of the two.
    //
    // ⚠️ AND THE AMBIENT/DIRECT SPLIT IS NOT A BRIGHTNESS DIAL, WHICH IS EASY TO
    // MISREAD FROM THE LINE BELOW. `g_sunAndAmbient.w` divides a fixed budget
    // between the ambient and the sun -- the sun is scaled by (1 - ambient) --
    // so simply forcing ambient to 1 for the environment path would silently
    // switch the sun OFF. The two terms are therefore weighted separately from
    // here on.
    bool envEnabled = g_envParams.z > 0.5;

    // The complete ambient contribution, weighting already applied.
    float3 ambientTerm = ambient * ambientColor;
    // What the sun's own term is scaled by.
    float  directWeight = 1.0 - ambient;

    if (envEnabled)
    {
        // The SH already carries the sky's measured magnitude, so it is used at
        // full strength and `intensity` is the only multiplier.
        ambientTerm = ShDiffuse (n) * g_envParams.x;

        // ⚠️ THE SUN IS HELD BACK WHEN A SKY IS ACTIVE, BECAUSE THE TWO OVERLAP.
        // This used to be `directWeight = 1.0` -- the sun at FULL strength, up
        // from the 1-ambient it gets otherwise, on top of an SH ambient already
        // carrying the sky's own measured magnitude. The result was reported
        // simply as "too bright" from the first real run (2026-08-17), which is
        // exactly the symptom the old comment here predicted and then shipped
        // anyway.
        //
        // ⚠️ THE WEIGHT IS A HUD SLIDER, NOT A CONSTANT, and that is the honest
        // shape for it. The correct fix is to find the sun disc in the HDR and
        // subtract it before projecting the SH, so the analytic sun REPLACES
        // rather than joins it -- that is its own piece of work. Until then the
        // right value genuinely depends on the sky in use, so it is exposed
        // (default 0.55) instead of being a magic number nobody can check.
        directWeight = saturate (g_materialParams.y);
    }

    // ---- RE51.C3: ambient occlusion ---------------------------------------
    //
    // ⚠️ IT MULTIPLIES THE AMBIENT TERM AND NOTHING ELSE. Ambient occlusion
    // describes how much of the SKY a point can see; the sun is a single
    // direction and already has a shadow map, which is a far better answer for
    // it. Multiplying the sun by AO too is the classic over-darkening that
    // makes a render look dirty rather than grounded, and it double-counts the
    // occlusion the shadow map has already applied.
    //
    // ⚠️ AND IT IS THE REASON MASSING STOPS FLOATING. Nothing else in this
    // shader darkens where a block meets the ground: the shadow map only knows
    // about the sun's direction, so a courtyard in full skylight had exactly
    // the brightness of an open roof. That contact gradient is what the eye
    // reads as "this object is SITTING on that surface".
    if (g_gradeParams.w > 0.0)
    {
        float occlusion = g_ambientOcclusion.Load (int3 (int2 (psIn.position.xy), 0));
        // lerp toward the measured occlusion by the intensity, so 0 is exactly
        // the image without this and 1 is GTAO at the strength it computed.
        // ⚠️ INTENSITY IS NOT THE EFFECT'S RADIUS. C3's acceptance asks for the
        // two to be separable; a radius change is a different request and lives
        // in the AO pass's own settings.
        ambientTerm *= lerp (1.0, saturate (occlusion), saturate (g_gradeParams.w));
    }

    // ---- direct: the sun, occluded by the shadow map ----------------------
    float ndotl  = max (dot (n, g_sunAndAmbient.xyz), 0.0);
    // ⚠️ psIn.position IS ALREADY IN PIXELS HERE. The hardware divided by w and
    // mapped SV_POSITION to the viewport before the pixel shader saw it, which is
    // exactly the space the PCSS dither wants: one distinct phase per screen
    // pixel, independent of how many pixels a shadow texel covers.
    float4 shadow = SampleSunShadow (psIn.worldPos, n, psIn.position.xy);
    float direct = directWeight * ndotl * shadow.x;

    float3 lighting = ambientTerm + direct.xxx * g_skyColor.w;
    float4 base = psIn.color * g_baseColor;

    // ---- RenderQuality::Realistic (PLAT-RE126) ----------------------------
    //
    // ⚠️ A BETTER SHADING MODEL, NOT A PHYSICALLY BASED RENDERER. Same forward
    // pass, same single cascade, no IBL and no G-Buffer -- those arrive with
    // DiligentFX and REPLACE what is below without the switch above it moving.
    // Three things carry almost all of the visible difference:
    //
    //   1. a specular highlight, so a surface reads as a MATERIAL rather than as
    //      a flat colour and the eye can find the geometry's curvature;
    //   2. a wrapped diffuse term, which is what stops the shadowed side of a
    //      massing block from going to a dead constant;
    //   3. tone mapping, so a sunlit white wall ROLLS OFF instead of clipping to
    //      flat white and losing its edges against the sky -- the single biggest
    //      reason the Fast look reads as "computer graphics".
    // Whether base.rgb already carries the alpha the premultiplied blend
    // expects. Only the Realistic branch premultiplies, because only it splits
    // the surface into a transmitted and a reflected half; the Fast path has no
    // reflection to protect and premultiplies in one step at the output.
    bool premultiplied = false;
)hlsl";

// The classifier's metalness and split-sum fit pushed the entry-point literal
// over MSVC's 16 KB limit. Keep the split at a statement boundary so the
// runtime source remains one shader while each C++ literal stays compilable.
constexpr const char* kArchVizMeshPSMainTail = R"hlsl(
    if (g_outline.z > 0.5)
    {
        // ---- GGX, driven by the material's OWN finish ----------------------
        //
        // ⚠️ THE ROUGHNESS WAS ALREADY BEING EXTRACTED AND THROWN AWAY. This
        // used to be one fixed Blinn-Phong lobe (exponent 48, strength 0.25)
        // applied to every surface, on the stated grounds that Archicad supplies
        // no roughness channel -- and that was wrong: `ModelerAPI::GetShining()`
        // has been read per surface into `SurfaceMaterial::shininess` since the
        // material pool was written, with its own header comment saying the
        // shader ignores it "until Phase 9 brings a specular term". The specular
        // term arrived and never collected it. So window glass, brushed metal
        // and rough concrete all carried the SAME highlight, which is the single
        // most "computer-generated" thing left in the picture once the shadows
        // and the ambient are right: a real facade is read by how differently
        // its materials catch the sun, and one lobe erases exactly that.
        //
        // ⚠️ DIELECTRIC ONLY -- THERE IS STILL NO METALNESS, only a varying F0.
        // Archicad's surface has no metalness channel (`GetShining` is a
        // shininess percentage, not a conductor flag), and deriving one from
        // shininess would paint every polished floor and every pane of glass as
        // a metal. That is a guess dressed as physics, and it is the kind that
        // looks plausible in one scene and wrong in every other. A real
        // metalness needs a material-preset table (the task spec's "architectural
        // presets"), not an inference here.
        //
        // What DID arrive is Archicad's second finish channel. `shining` says
        // how TIGHT the highlight is (roughness, above); `specularReflection`
        // says how MUCH light leaves specularly, and SurfaceFinishProbe measured
        // 17 distinct values of it against 12 of shining on the same pool -- so
        // it carries more of this project's material variety than the channel
        // the renderer was already using, and it was being thrown away.
        // ⚠️ THE BIAS IS ADDED BEFORE THE CLAMP, so the 0.045 floor still
        // protects the GGX denominator no matter how far it is pushed.
        float  rough = clamp (g_outline.w + g_gradeParams.z, 0.045, 1.0);
        float  alpha = rough * rough;

        float3 h     = normalize (g_sunAndAmbient.xyz + v);
        float  ndoth = saturate (dot (n, h));
        float  ndotv = saturate (dot (n, v));
        float  vdoth = saturate (dot (v, h));

        // Trowbridge-Reitz (GGX) normal distribution.
        float  denom = ndoth * ndoth * (alpha * alpha - 1.0) + 1.0;
        float  ndf   = (alpha * alpha) / max (3.14159265 * denom * denom, 1e-7);

        // Smith-Schlick visibility, Unreal's direct-light k, with the BRDF's own
        // 1/(4 NdotL NdotV) folded in -- so the NdotL below is the rendering
        // equation's cosine and NOT a second copy of the geometry term.
        float  k    = (rough + 1.0) * (rough + 1.0) * 0.125;
        float  visV = ndotv * (1.0 - k) + k;
        float  visL = ndotl * (1.0 - k) + k;
        float  vis  = 0.25 / max (visV * visL, 1e-5);

        // ⚠️ 0.08 IS CHOSEN SO THAT THE DEFAULT CHANGES NOTHING. Unreal's
        // remapping of a 0..1 artist "specular" onto dielectric F0 is
        // F0 = 0.08 * specular, which puts the midpoint 0.5 exactly on 0.04 --
        // the fixed value every surface used before this channel was read. So a
        // project whose surfaces all sit at the default renders identically, and
        // only genuine authored variation moves. A plain `specular * 0.04` would
        // have halved the highlight on the entire model and read as a
        // regression in the lighting.
        float  f0scalar = 0.08 * saturate (g_materialParams.x);

        // ⚠️ METALNESS ARRIVES FROM THE CLASSIFIER, NOT FROM SHININESS. It is
        // uploaded per range in g_materialParams.z by SurfaceClassifier's
        // PresetFor; Archicad itself has no such channel, and deriving one from
        // shine would paint every polished floor and every pane as chrome. The
        // mapping below is the glTF metallic-roughness convention as DiligentFX
        // implements it (Shaders/PBR/public/PBR_Shading.fxh:438-440): a
        // conductor's F0 IS its colour, and it has no diffuse lobe at all.
        float  metallic = saturate (g_materialParams.z);
        float3 f0       = lerp (float3 (f0scalar, f0scalar, f0scalar), base.rgb, metallic);
        float3 fresnel  = f0 + (1.0 - f0) * pow (1.0 - vdoth, 5.0);
        float3 spec     = ndf * vis * fresnel * ndotl * shadow * g_gradeParams.y;

        // Wrapped diffuse: light bends a little past the terminator. Physically
        // this stands in for the bounce a single directional light cannot carry.
        float  wrapped = saturate ((dot (n, g_sunAndAmbient.xyz) + 0.3) / 1.3);
        float  directSoft = directWeight * wrapped * shadow;

        lighting = ambientTerm + directSoft.xxx * g_skyColor.w;

        // ---- the grazing sky reflection ------------------------------------
        //
        // ⚠️ THIS IS WHAT MAKES GLASS READ AS GLASS, and it is a SEPARATE
        // Fresnel from the sun's. The specular above answers "where is the sun's
        // reflection"; a window is mostly not pointing at the sun, and what it
        // actually shows is the SKY, most strongly at glancing angles. Using the
        // sun's half-vector Fresnel for this would put the grazing sheen only
        // where the highlight already is, which is to say nowhere useful.
        //
        // ⚠️ TWO DIFFERENT THINGS SHARE THIS BRANCH, and only the fallback is
        // an approximation. With an HDR loaded this samples the GGX-PREFILTERED
        // environment (RE51.B6) along the reflected view direction -- a real
        // IBL specular term. Without one it reuses the two hemisphere colours
        // the ambient already has, which costs no texture and no precompute and
        // is a two-colour stand-in for a sky, not a prefiltered environment.
        float3 refl = reflect (-v, n);
        float3 envColor;
        if (envEnabled)
        {
            // ⚠️ THE MIP IS NOW A REAL GGX LOBE (RE51.B6), AND THE MAPPING
            // CHANGED WITH IT. Each mip of g_envMap is rendered by
            // kArchVizEnvPrefilterPS at perceptual roughness i/(mips-1), so the
            // index is LINEAR in perceptual roughness by construction. It used
            // to be sqrt(rough), which was a correction for the box mip chain
            // being far too sharp for its nominal roughness -- keeping that
            // curve now would over-blur every glossy surface by the same amount
            // it used to under-blur it.
            //
            // ⚠️ DO NOT REINTRODUCE A CURVE HERE. If a surface reads as too
            // sharp or too blurred, the prefilter's roughness ladder is the
            // thing that is wrong, and it is one line in EnvironmentMap.cpp.
            // A second mapping here would make the two disagree silently.
            float mip = saturate (rough) * max (g_envParams.w - 1.0, 0.0);
            envColor = g_envMap.SampleLevel (g_envMap_sampler, EnvUv (refl), mip).rgb
                     * g_envParams.x;
        }
        else
        {
            envColor = lerp (g_groundColor.rgb, g_skyColor.rgb,
                             saturate (refl.z * 0.5 + 0.5));
        }
        // ⚠️ THE SPLIT-SUM ENVIRONMENT BRDF, WHICH REPLACED A TERM THAT WAS
        // SILENTLY DELETING EVERY REFLECTION IN THE MODEL.
        //
        // What stood here was `fresnelEnv * envColor * (1-rough)^2`. That
        // squared falloff is not physics -- it double-counts the roughness,
        // which the mip selection above has ALREADY applied -- and on the
        // measured project it is what made the symptom. With the roughness
        // mapping corrected, the glossiest surface in the building sits at
        // roughness 0.257, and (1-0.257)^2 = 0.55; before that correction it sat
        // at 0.718 and the factor was 0.079. Either way, concrete at roughness
        // 0.82 kept 3% and everything matte kept none, so the model rendered
        // with no environment reflection anywhere and the fix for the roughness
        // alone did not bring it back.
        //
        // The correct quantity is the second half of Karis's split-sum: the
        // integral of the GGX BRDF over the hemisphere, tabulated against NdotV
        // and roughness, giving a scale and a bias for F0. DiligentFX samples it
        // from a precomputed LUT (Shaders/PBR/private/PrecomputeBRDF.psh, used
        // in PBR_Shading.fxh as `SpecularLight * (F0 * PreIntBRDF.x +
        // PreIntBRDF.y)`). ⚠️ THAT LUT IS RE51.B6'S JOB, not this one -- it
        // needs a precompute pass and a second texture binding. This is the
        // ANALYTIC FIT to the same table (Karis 2013, "Physically Based Shading
        // on Mobile"), which needs neither, and which B6 should replace with the
        // real LUT rather than delete.
        //
        // ⚠️ THE BIAS TERM IS WHY GLASS WORKS. `AB.y` is the grazing-angle
        // reflection that survives even when F0 is 0.04, and it is exactly the
        // quantity the old squared falloff scaled to nothing.
        float4 c0 = float4 (-1.0, -0.0275, -0.572, 0.022);
        float4 c1 = float4 ( 1.0,  0.0425,  1.040, -0.040);
        float4 rp = rough * c0 + c1;
        float  a004 = min (rp.x * rp.x, exp2 (-9.28 * ndotv)) * rp.x + rp.y;
        float2 envAB = float2 (-1.04, 1.04) * a004 + rp.zw;
        float3 envBrdf = f0 * envAB.x + envAB.y;

        float3 envSpec = envBrdf * envColor * g_gradeParams.y;

        // ⚠️ STILL NEEDED SEPARATELY, for the diffuse side only. `envBrdf`
        // above is the specular share; `fresnelEnv` below is what that share
        // takes AWAY from the diffuse, and the two are not the same number.
        float3 fresnelEnv = f0 + (1.0 - f0) * pow (1.0 - ndotv, 5.0);

        // Energy conservation: what reflects specularly does not also diffuse.
        // At normal incidence this removes 4% and is invisible; at grazing
        // angles it is what stops a shiny surface from being BRIGHTER than a
        // matte one instead of merely different.
        //
        // ⚠️ AND A CONDUCTOR HAS NO DIFFUSE LOBE AT ALL. Metals absorb what they
        // do not reflect, so `(1 - metallic)` removes the diffuse term outright
        // rather than dimming it -- the second half of the same glTF convention
        // that put the base colour into F0 above (PBR_Shading.fxh:438). Without
        // this a metal would be a diffuse surface WITH a mirror on top and read
        // as painted plastic, which is the classic way metalness looks wrong.
        float3 kd = (1.0 - fresnelEnv) * (1.0 - metallic);

        // ⚠️ THE TRANSMITTED HALF IS SCALED BY ALPHA AND THE REFLECTED HALF IS
        // NOT. This is the shader's side of the premultiplied blend set up in
        // DiligentScene::Init, and the split is the whole point: `base.rgb` is
        // what you see THROUGH the pane, which a clearer pane shows less of,
        // while `spec + envSpec` bounces OFF its front face and does not care
        // what is behind it. Scaling both -- which SRC_ALPHA blending did for
        // free -- made this project's 69%-transparent glass reflect at under a
        // third strength and get dimmer the clearer it was authored.
        float3 colour = base.rgb * lighting * kd * base.a +
                        (spec + envSpec) * g_skyColor.w;

        // ACES, the fitted form. ⚠️ SEE AcesFitted IN THE PRELUDE for why this is
        // no longer a per-channel curve: the per-channel one was pushing the
        // channels of an already-saturated surface further apart, which is what
        // made this project's greens and oranges read as over-saturated once the
        // sRGB decode (RE51.B7) stopped washing them toward grey.
        //
        // ⚠️ CONDITIONAL ON g_frameControl.x: when rendering into the HDR
        // scene-colour target, Grade is SKIPPED here and applied once by the
        // resolve pass. That is what makes the frame scene-referred rather than
        // display-referred, and it is what C7's SSR and B9's percentile exposure
        // need as input. When g_frameControl.x is 0 (the LDR path, Fast quality,
        // or any debug view), the shader tone-maps exactly as before.
        if (g_frameControl.x > 0.5)
            base.rgb = colour;
        else
            base.rgb = Grade (colour);
        lighting = float3 (1.0, 1.0, 1.0);   // already applied above
        premultiplied = true;                // and so was the alpha
    }

    // The debug ladder. See DiligentDebugView for what each one proves; the
    // int() of a float is exact for these small values.
    int view = int (g_eyePos.w);
    if (view == 1)
        psOut.color = float4 (n * 0.5 + 0.5, 1.0);
    else if (view == 2)
        psOut.color = float4 (lighting, 1.0);
    else if (view == 3)
        psOut.color = float4 (base.rgb, 1.0);
    else if (view == 4)
        psOut.color = float4 (g_sunAndAmbient.xyz * 0.5 + 0.5, 1.0);
    else if (view == 5)
        psOut.color = float4 (shadow.xxx, 1.0);
    else if (view == 6)
    {
        // ⚠️ THE RAW UPLOADED VALUE, NOT THE CLAMPED ONE the GGX branch uses,
        // and not gated on Realistic. This view answers "did the material
        // channel arrive", which is a question about the CONSTANT BUFFER -- so
        // showing it only where the lighting happens to consume it would hide
        // the case where the upload is wrong, and clamping it would hide a zero.
        float r = saturate (g_outline.w);
        psOut.color = float4 (r, r, r, 1.0);
    }
    else
    {
        // ⚠️ THE BLEND IS PREMULTIPLIED, so an un-premultiplied colour here
        // comes out at full brightness over the background instead of fading
        // with the surface -- glass that is somehow MORE opaque than the wall
        // behind it. The opaque pass is unaffected either way: alpha is 1 there
        // and the multiply is the identity.
        float3 shaded = premultiplied ? base.rgb : base.rgb * lighting * base.a;
        // Apply diagnostics after both quality paths. Realistic has already
        // folded lighting into base.rgb, so applying these earlier is silently
        // overwritten by its GGX branch.
        if (g_shadowAttribs.bVisualizeShadowing != 0)
            shaded = shadow.xxx;
        if (g_shadowAttribs.bVisualizeCascades != 0)
            shaded = lerp (shaded, shadow.yzw, 0.18);
        psOut.color = float4 (shaded, base.a);
    }
}
)hlsl";

// One shader's full source: the shared cbuffer, then the stage's own body.
//
// Additional pieces are optional and exist only because MSVC caps a string
// literal at 16 KB -- see the split mesh pixel shader. Passing the pieces
// separately keeps the limit a property of THIS file rather than something
// every caller has to know about.
inline std::string ArchVizShaderSource (const char* body, const char* more = nullptr, const char* evenMore = nullptr,
                                        const char* last = nullptr, const char* final = nullptr)
{
    std::string source = std::string (kArchVizCBuffer) + body;
    if (more != nullptr)
        source += more;
    if (evenMore != nullptr)
        source += evenMore;
    if (last != nullptr)
        source += last;
    if (final != nullptr)
        source += final;
    return source;
}

} // namespace archviz
} // namespace geomsrv

#endif
