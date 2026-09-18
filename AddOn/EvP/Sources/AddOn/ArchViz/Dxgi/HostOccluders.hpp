#ifndef EVP_ARCHVIZ_DXGI_HOSTOCCLUDERS_HPP
#define EVP_ARCHVIZ_DXGI_HOSTOCCLUDERS_HPP

// The building's OPAQUE surfaces, re-rendered depth-only from Archicad's own GPU
// camera, as the thing that hides the overlay (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 9).
//
// ⚠️ THIS IS THE SEPARATION FOUR RUNS PAID FOR. Runs forty-eight to fifty-one
// each tried to make Archicad's own depth-stencil view answer "is the overlay
// behind something", and each failed for the same reason in a new place:
//
//     forty-eight  26% of a primitive IN FRONT of the model was rejected
//     forty-nine   all 2914 scene draws report `BlendEnable = TRUE`, so the
//                  opaque/transparent boundary is not in the D3D state at all
//     fifty        every sampled checkpoint read GOOD while the screen was wrong
//     fifty-one    located it: after draw #2 the ghost loses 66% of its pixels
//                  to a six-index quad that does not even write depth
//
// Archicad's buffer is the truth about Archicad's PIXELS. A transparent build
// plane is in those pixels, and a designer does not mean it when they say
// "behind the wall". No sampling moment can fix that, because the information
// was never in the buffer.
//
// ⚠️ SO THE QUESTION IS ANSWERED WHERE THE ANSWER LIVES. The GPU hook says WHERE
// the camera is and WHEN to compose. The model says WHAT is opaque --
// `SurfaceMaterial::alpha` against `kOpaqueAlpha`, which `MaterialTable` already
// measures from the real template and `SurfaceClassifier` already reasons about.
// The classification therefore happens in `SceneCmdQueue`, which holds both the
// element and the material table, and THIS file never sees a material at all: it
// receives triangles that are already known to be opaque and renders them.
//
// ⚠️ THE CONCEPT IS ALREADY PROVEN IN THIS TREE. `ArchViz/OcclusionPrepass`
// renders Archicad's model depth-only from the synced camera for the portable
// overlay, for exactly this reason. What is new here is the opacity filter and
// the fact that it runs on ARCHICAD'S OWN DEVICE, inside Archicad's frame, with
// Archicad's own uploaded camera -- so there is no camera lag to add.
//
// ⚠️ AND THE LIMIT IS REAL: THIS OCCLUDES AGAINST ARCHICAD'S *MODEL*, NOT ITS
// *PIXELS*. An element the extraction never saw cannot hide the overlay, and a
// stale extraction hides it in the wrong place. That is a worse failure than
// using the raw buffer in exactly one respect and better in every other, and it
// is the trade this stage makes deliberately.
//
// THREAD SAFETY. `BeginBatch`/`AddOpaqueTriangles`/`EndBatch` run on the
// extraction thread. `Prepare` runs on Archicad's render thread inside a detour.
// ⚠️ THEY NEVER SHARE A LOCK. The producer builds a complete snapshot and
// publishes it with one atomic exchange; the render thread takes ownership of it
// with another. A mutex on the Present path would let a slow extraction stall
// Archicad's frame, which is the one thing this whole rung may never do.

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace hostocclusion {

// ---- extraction thread ------------------------------------------------------
// MAIN/EXTRACTION THREAD. Start accumulating a new set of opaque host surfaces.
// `full` discards what was there; otherwise the batch adds to it.
void BeginBatch (bool full);

// MAIN/EXTRACTION THREAD. Add one element's vertex block and return the base to
// offset its indices by, or `kNoBase` when it would not fit.
//
// ⚠️ VERTICES ONCE PER ELEMENT, INDICES ONCE PER OPAQUE RANGE, AND THE
// SPLIT IS NOT COSMETIC. An element is material-grouped into several ranges; the
// first version of this took the whole vertex array alongside each range and so
// copied every element's geometry once PER MATERIAL. On a real model that is a
// multiple-times blow-up of both memory and extraction time, and it filled the
// ceiling with duplicates of the same wall.
//
// Positions are world metres, xyz interleaved, in Archicad's own coordinates --
// the same numbers `ElementUpload::vertices` carries, because the overlay's
// camera is Archicad's and expects nothing else.
constexpr uint32_t kNoBase = 0xffffffffu;
uint32_t AddVertices (const float* xyz, uint32_t vertexCount);

// MAIN/EXTRACTION THREAD. ⚠️ OPAQUE RANGES ONLY, AND THE CALLER HAS
// ALREADY DECIDED THAT. See the header note: this file never sees a material.
void AddOpaqueIndices (uint32_t vertexBase, const uint32_t* indices, uint32_t indexCount);

// EXTRACTION THREAD. Triangles of a TRANSPARENT surface: glass, a build plane, a
// helper.
//
// ⚠️ THEY DO NOT OCCLUDE AND THEY ARE STILL THE BUILDING.
// Invariant 9 excludes transparent surfaces from OCCLUSION -- it does not say
// they are not there. Dropping them entirely meant that turning a slab to glass
// removed it from the reference wireframe as well: the geometry existed in
// Archicad, the user could see it, and the overlay drew nothing where it was.
//
// So the split is between the two USES, not at the input: the occluder renders
// opaque triangles only, and the feature-edge pass that makes the wireframe sees
// opaque and transparent alike.
void AddTransparentIndices (uint32_t vertexBase, const uint32_t* indices, uint32_t indexCount);

// MAIN/EXTRACTION THREAD. ⚠️ WHAT WAS REJECTED IS EVIDENCE TOO. A run
// with zero opaque triangles and zero transparent ones means the extraction
// produced nothing; zero opaque and many transparent means the classifier is
// wrong or the model really is all glass. Those are different faults and a
// single "no geometry" cannot tell them apart.
void NoteTransparent (uint32_t indexCount);

// MAIN/EXTRACTION THREAD. Publish what was accumulated. Until this is called the
// render thread keeps using the previous snapshot, so a half-extracted building
// never occludes anything.
void EndBatch ();

// MAIN THREAD. Forget the host entirely; the overlay stops being occluded.
void Clear ();

// ---- render thread ----------------------------------------------------------
// RENDER THREAD, at the injection point, under `ScopedInjectionGuard`.
//
// Clears a private depth buffer to far, renders every opaque host triangle into
// it with the injected camera, and returns the view. ⚠️ THE OVERLAY THEN DRAWS
// INTO THIS SAME VIEW: the host's depth values are what hide it behind the
// building, and its own writes are what hide it behind itself. One buffer, two
// meanings, because D3D11 binds one depth-stencil view.
//
// Returns null when there is no host geometry, no camera, or the device objects
// could not be made -- never a half-rendered buffer, because a partially drawn
// occluder hides the overlay in a pattern that looks like a transform bug.
// ⚠️ `targetWidth`/`targetHeight` ARE THE SURFACE THIS WILL BE
// BOUND BESIDE, AND THEY ARE NOT OPTIONAL. D3D11 refuses a binding whose render
// target and depth-stencil differ in size by even ONE PIXEL, and Archicad's
// windowed swap chain measured 2450 wide against a 2449-wide 3D view. The
// overlay then drew nothing while every counter said it had -- visible in full
// screen, where the two happen to be equal, and invisible in a window.
//
// This buffer is OURS: the host geometry is rendered into it from the extracted
// model with the injected camera, so nothing about it needs Archicad's
// dimensions. Only the FORMAT and SAMPLE COUNT are taken from the scene view, so
// the depth values remain comparable. Passing 0 keeps the old behaviour of
// matching the scene view exactly.
ID3D11DepthStencilView* Prepare (ID3D11DeviceContext* context, ID3D11DeviceContext1* context1, uint32_t interpretation,
                                 uint32_t targetWidth, uint32_t targetHeight);

// MAIN THREAD, at the start of an overlay session. Zero ONLY the render-thread
// refusal counters. ⚠️ THE PUBLISHED SNAPSHOT DELIBERATELY
// SURVIVES: the building has not changed because the overlay was toggled, and
// re-extracting it would cost a full model walk for nothing. What must not
// survive is a refusal from the previous session, which `BlockedAt` would report
// as this one's.
void ResetRenderCounters ();

// MAIN THREAD, from the runtime heartbeat: the model revision Archicad is
// currently at. `modelwatch::Stats::geometryEdits` is that number -- monotonic,
// bumped once per reported element change, and deliberately NOT bumped by
// navigation.
//
// ⚠️ THE THREE REVISIONS ARE THE WHOLE DIAGNOSIS. A batch
// stamps the value it began with, publication carries it, and the GPU upload
// records it again:
//
//     model=42 published=42 gpu=42   the overlay is the building
//     model=42 published=41 gpu=41   an edit was never extracted
//     model=42 published=42 gpu=41   extracted but not uploaded
//
// Without them "the overlay looks old" is a conversation; with them it is a
// line in the log that names which stage stopped.
void SetModelRevision (uint32_t revision);

// RENDER THREAD, after `Prepare` has returned non-null. The published building,
// lent rather than copied, so an overlay can DRAW the same triangles the
// occluder just put depth from.
//
// ⚠️ LENT, NOT HANDED OVER. These are this module's buffers and it
// keeps owning them; a borrower binds them for one draw and adds no reference.
// The pattern is `InjectionProbes::GetWorldProbePipeline`, and the reason is the
// same: two modules that each built their own copy of Archicad's model would
// drift apart the first time one of them changed its vertex format.
//
// ⚠️ `scalars` IS ONE FLOAT PER VERTEX IN A SECOND SLOT, NOT A
// WIDER VERTEX. The occluder is a depth-only pass over a whole building and has
// no use for the channel; interleaving it would move a third more bytes through
// the most expensive draw on this path to feed the cheapest one.
struct HostGeometry {
    ID3D11Buffer* positions = nullptr; // float3, slot 0
    ID3D11Buffer* scalars = nullptr;   // float, slot 1 -- see `ScalarField`
    ID3D11Buffer* indices = nullptr;   // R32_UINT, triangle list
    uint32_t indexCount = 0;

    // ⚠️ THE EDGES ARE A SEPARATE INDEX BUFFER, NOT A FILL MODE.
    // `D3D11_FILL_WIREFRAME` over a triangle list draws EVERY triangle edge,
    // including the diagonal across each flat quad -- so a plain wall comes out
    // as a mesh of triangles rather than a rectangle, which is the opposite of a
    // reference overlay. These are FEATURE edges: boundaries, and creases whose
    // dihedral angle exceeds `kCreaseCosine`. See `EndBatch`.
    ID3D11Buffer* lines = nullptr; // R32_UINT, line list
    uint32_t lineIndexCount = 0;

    // ⚠️ THE OUTLINE OF A CURVED SURFACE IS NOT IN THE
    // GEOMETRY. A cylinder has no crease down its side -- every facet boundary is
    // 15 degrees on a 24-segment column -- so the feature edges above find its two
    // cap rings and nothing else, and a column arrives as two circles floating in
    // space. Which side of it you can see depends on where you are standing, so
    // it is decided per frame, in the vertex shader, from the two adjacent face
    // normals this buffer carries beside each position.
    //
    // NOT indexed: the normals belong to the EDGE, and a vertex shared by several
    // curved edges has a different pair for each. Nine floats per vertex, two
    // vertices per edge, drawn as a plain line list.
    ID3D11Buffer* silhouette = nullptr;
    uint32_t silhouetteVertexCount = 0;

    bool valid = false;

    // ⚠️ WHICH WAY THE EXTRACTION WINDS ITS TRIANGLES, MEASURED
    // RATHER THAN ASSUMED. A consumer that wants to cull back faces -- a surface
    // heatmap does, so the inside of the far wall does not paint over the near
    // one -- has to know this, and guessing it wrong culls the ENTIRE building
    // and looks exactly like the overlay being broken. See `EndBatch`: the
    // signed volume of the published mesh answers it in one pass, on the
    // producer, where the geometry is.
    bool frontCounterClockwise = true;
    bool windingKnown = false;
};
HostGeometry GetGeometry ();

// ⚠️ WHAT THE SCALAR MEANS, DECIDED WHERE THE MODEL IS, NOT IN A
// SHADER. `Height` normalises world Z over the published snapshot's own range,
// which is a defensible default and demonstrably not a placeholder colour ramp.
// A real analysis field -- daylight, utilisation, thermal -- arrives the same
// way: the extraction computes one float per vertex and nothing downstream
// changes. That is the whole point of putting it on the producer side.
enum class ScalarField { Height };

// ⚠️ THREE GROUPS, AND MIXING THEM COST A RUN. Run fifty-six read
// `batchEnds 130, opaqueTriangles 19760` beside `haveSnapshot false, triangles 0`
// and concluded the publication had failed. It had not: those last two were
// RENDER-THREAD upload counters, and the render thread only runs when injection
// is active -- which the run had deliberately ordered AFTER extraction. The gate
// was unreachable by construction and the wait ran to its full timeout.
//
// So the groups are named for the thread that writes them, and the question
// "did the model reach the overlay" is answered ONLY by the producer group.
struct Stats {
    // ---- the extraction thread: what arrived ----------------------------
    uint64_t extractionGeneration = 0; // ++ at each BeginBatch
    uint64_t batchBegins = 0;
    uint64_t batchEnds = 0;
    uint64_t elementsReceived = 0;
    uint64_t opaqueVerticesAdded = 0;
    uint64_t opaqueIndicesAdded = 0;
    uint64_t opaqueTriangles = 0;
    uint64_t transparentTrianglesSkipped = 0;
    uint64_t droppedOverCapacity = 0;
    uint32_t pendingVertices = 0; // this batch, so far

    // ---- the extraction thread: what was handed over --------------------
    // ⚠️ `haveSnapshot` IS STICKY AND THAT IS DELIBERATE. `TakeAndUpload`
    // exchanges the pending snapshot away, so "is one pending" goes false the
    // moment the GPU accepts it -- which would read as the model having been
    // lost at the exact moment it arrived.
    uint64_t publishAttempted = 0;
    uint64_t publishSucceeded = 0;
    uint64_t publishedGeneration = 0;
    uint32_t publishedVertices = 0;
    uint32_t publishedIndices = 0;
    uint32_t publishedTriangles = 0;
    uint32_t publishedLines = 0;
    // Curved edges offered to the per-frame test. How many of them DRAW is a
    // property of the camera and changes every frame, so it is not counted here.
    uint32_t publishedSilhouetteVertices = 0; // feature edges; see `HostGeometry::lines`
    uint32_t edgesConsidered = 0;             // unique welded edges, before the crease test

    // ⚠️ WHERE THE BUILDING IS, WHICH IS THE ONLY RELIABLE ANCHOR FOR
    // THE CAMERA CENSUS. The census scores candidates by projecting a world-space
    // triangle and needs a point that is certainly on screen when the user is
    // looking at their model. Archicad's own orbit target would be ideal and IS
    // NOT AVAILABLE IN AN AXONOMETRIC VIEW -- `Get3DProjectionSets` reports
    // `isPersp == false`, there is no eye and no target at all, and the reader
    // correctly returns "invalid". That is how run sixty-two spent itself in
    // `Learning` with the anchor still at the world origin. The extracted model
    // always has a centre.
    bool boundsValid = false;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    // See `HostGeometry::frontCounterClockwise`. `signedVolume` is reported so a
    // near-zero magnitude -- an open mesh, where the sign means nothing -- can
    // be recognised rather than trusted.
    float signedVolume = 0.0f;
    bool windingKnown = false;
    bool frontCounterClockwise = true;
    bool haveSnapshot = false;
    char publishFailureReason[160] = {};

    // ---- the render thread: what is on the GPU --------------------------
    uint64_t uploads = 0;
    uint64_t renders = 0;
    uint64_t skippedNoCamera = 0;
    uint64_t skippedNoGeometry = 0;
    uint64_t skippedNoDepthTarget = 0; // no scene depth view to match; see Prepare
    // See `SetModelRevision`. Equal is healthy; falling behind names the stage.
    uint32_t modelRevision = 0;
    uint32_t publishedRevision = 0;
    uint32_t gpuRevision = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t triangles = 0;
    bool uploaded = false;
    uint32_t width = 0, height = 0;
    bool ready = false;
    char lastError[160] = {};
};
Stats GetStats ();

// MAIN THREAD, at teardown.
void Shutdown ();

} // namespace hostocclusion
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
