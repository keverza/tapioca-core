#ifndef EVP_ARCHVIZ_DXGI_INJECTIONCAMERA_HPP
#define EVP_ARCHVIZ_DXGI_INJECTIONCAMERA_HPP

// The camera the injected primitive is drawn with: which draws it is taken
// from, how its 256 bytes are preserved, and which reading of those bytes the
// shader implements (PLAT-RE155,
// docs/architecture/api/HANDOFF-OverlayPatch.md stage 5).
//
// ⚠️ THIS IS SPLIT OUT OF `InjectionRenderer` ALONG THE SEAM THE LAST FIVE RUNS
// REVEALED. That file answered two questions at once -- what the camera IS, and
// how the primitive is DRAWN -- and every failure since run twenty-eight has
// been in the first while the second was never in doubt. The clip-space probe
// has been rock solid throughout; the camera has been the wrong one, the
// overwritten one, the unconnected one and the wrongly-transposed one in turn.
// Two questions, two files.
//
// ⚠️ THE BINDING IS NOT THE BYTES. `buffer + firstConstant + numConstants` says
// WHERE the camera lived when the model was drawn; it does not preserve WHAT was
// there. Archicad's constants live in one advancing 8 MiB ring, so the 256 bytes
// are copied GPU-to-GPU into buffers we own, at the moment the draw that
// consumes them happens. No `Map`, no CPU readback, no synchronisation.
//
// ⚠️ AND THE READING OF THOSE BYTES IS MEASURED, NOT ASSUMED. The census scores
// eight interpretations on every moving frame; the shader implements exactly one
// of them; `InterpretationAgrees` is what stops those two from silently
// differing, which is precisely what run thirty-three did for a whole run.

#include "ArchViz/Dxgi/ContextStateTracker.hpp"

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11DeviceContext;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {

// ⚠️ WHERE THE CAMERA COMES FROM, NAMED EXPLICITLY SO A SILENT FALLBACK CANNOT
// HIDE. `Learner` snapshots at every camera-bearing draw the learned model pass
// admits -- which is what put two different cameras into one pass and made the
// triangle vanish in motion. `CensusSelectedGroup` snapshots ONLY at draws
// matching the group the census committed to. `None` draws nothing at all,
// which is what a run with no identified camera must do.
enum class CameraSource { None, Learner, CensusSelectedGroup };
void         SetCameraSource (CameraSource source);
CameraSource GetCameraSource ();

// ⚠️ THE AUTHORITATIVE CAMERA WHEN A CENSUS GROUP IS SELECTED. Present reads
// THIS -- not `contextstate::LastCameraDraw`, not the learner's latch. Those
// paths stand down completely while a group is selected, because "mostly the
// selected group" is the same bug this rung has already spent five runs on.
struct SelectedCameraState {
    bool     valid = false;
    uint32_t groupId = 0;
    uint64_t snapshotGeneration = 0;
    uint64_t modelSceneGeneration = 0;
    uint64_t sourceDrawSequence = 0;
    float    viewportX = 0.0f, viewportY = 0.0f;
    float    viewportWidth = 0.0f, viewportHeight = 0.0f;
};
SelectedCameraState GetSelectedCamera ();

// RENDER THREAD, from a draw detour, for a camera-bearing draw of the LEARNED
// model pass. Stands down entirely once a census group is selected.
void SnapshotCamera (ID3D11DeviceContext* context);

// RENDER THREAD, from the census, for a draw that matched the selected group.
//
// ⚠️ IT DOES NOT CHECK `contextstate::Injecting ()`, AND THAT IS NOT AN
// OVERSIGHT. The census already holds `ScopedInjectionGuard` when it calls here
// -- it must, because its own staging copies would otherwise be recorded as
// Archicad's -- so a reentrancy check refuses EVERY call. Run thirty-two shipped
// with that check: the selected camera was never snapshotted, every Present
// classified INVALID_SCENE, and the log showed 0 injections with 0 skips. The
// guard nests, so the copy re-enters it safely.
void SnapshotSelectedDraw (ID3D11DeviceContext* context,
                           const contextstate::SceneDrawState& draw, uint32_t groupId);

// ⚠️ THE CONVENTION THE SHADER IMPLEMENTS, AND THE ONE THE CENSUS LEARNED, HELD
// SIDE BY SIDE SO THEY CANNOT SILENTLY DIFFER. When a group is selected and its
// learned interpretation is not the shader's, the injection REFUSES rather than
// drawing a transform nobody chose.
// ⚠️ NOT A CONSTANT ANY MORE, AND THAT IS THE POINT. It used to be a number
// written in the renderer asserting which reading the compiled shader
// implements -- a claim nobody could check, and run thirty-six showed it can be
// both unchecked and wrong. All four declarations are now compiled and the one
// the census selected is bound, so this REPORTS what is bound rather than
// promising it.
uint32_t ShaderInterpretation ();
void     SetShaderInterpretation (uint32_t variant);
void     SetExpectedInterpretation (uint32_t variant);   // 0xffffffff clears it
uint32_t ExpectedInterpretation ();
bool     InterpretationAgrees ();

// ⚠️ THE SELECTED GROUP DRAWS SEVERAL TIMES PER MODEL FRAME, AND ONLY ONE OF
// THOSE DRAWS CARRIES THE VISIBLE MODEL'S CAMERA. Run thirty-four's snapshot
// sequence climbed by SIX per model generation, and the oracle rows alternated
// between an anchor at NDC (-0.058, 0.080, z 0.50) and one at (-1.004, 0.994,
// z ~100) -- from the same group, in the same frame. Keeping one global snapshot
// meant the shader was handed whichever of the six drew last.
//
// So each occurrence within a model frame is kept, scored and ranked SEPARATELY,
// and exactly one of them is then locked as the camera.
//
// ⚠️ THE OCCURRENCE INDEX IS A POSITION, NOT AN IDENTITY DERIVED FROM THE
// BINDING. It is reset to zero whenever the model generation changes and
// incremented per matching draw within that generation. `firstConstant` is
// explicitly NOT used: Archicad's ring window advances every frame by design, so
// it identifies a moment, never a role.
constexpr size_t kOccurrenceCapacity = 8;

struct OccurrenceStats {
    uint32_t index = 0;
    uint64_t draws = 0;
    uint64_t modelFrames = 0;      // model generations this occurrence appeared in
    uint32_t samples = 0;          // readbacks that landed and were scored
    uint32_t insideClip = 0;       // ... of those, with a valid projection
    float    medianCentreError = 0.0f;
    float    meanCentreError = 0.0f;
    float    worstCentreError = 0.0f;
    float    meanSpreadPixels = 0.0f;   // see CameraCensus::Group
    float    viewportWidth = 0.0f, viewportHeight = 0.0f;
    uint64_t lastDrawSequence = 0;
};

// ANY THREAD. Newest statistics for every occurrence seen.
size_t   CopyOccurrences (OccurrenceStats* out, size_t capacity);
uint64_t OccurrenceModelFrames ();      // the coverage denominator

// MAIN THREAD. Clear the occurrence table without touching the lock.
void ResetOccurrences ();

// MAIN THREAD. Choose the occurrence that clears the gate and lock on to it.
// ⚠️ FAILS CLOSED: returns false and locks nothing when none qualifies, and an
// unlocked camera never becomes valid, so Present injects nothing rather than
// injecting with whichever occurrence drew last.
// ⚠️ THE CENSUS CHOOSES THE OCCURRENCE NOW, because the camera identity is
// atomic -- signature AND occurrence, scored together. This records what it
// chose; the local scoring below stays only as a report.
void     SetSelectedOccurrence (uint32_t index);

bool     SelectOccurrence ();
void     ClearOccurrenceLock ();
bool     OccurrenceLocked ();
uint32_t LockedOccurrence ();

// ---- what the renderer needs to bind and to report -------------------------
ID3D11Buffer* ViewSnapshotBuffer ();
ID3D11Buffer* ProjectionSnapshotBuffer ();
bool          SnapshotValid ();
contextstate::SceneDrawState SnapshotDraw ();
uint64_t      SnapshotModelGeneration ();
uint64_t      SnapshotDrawSequence ();

struct CameraStats {
    uint64_t qualifyingCameraDraws = 0;
    uint64_t viewCopies = 0;
    uint64_t projectionCopies = 0;
    uint64_t snapshotsTaken = 0;
    uint64_t selectedGroupDraws = 0;
    uint64_t selectedGroupSnapshots = 0;
    uint32_t selectedGroupId = 0;
    uint64_t selectedSnapshotGeneration = 0;
    bool     snapshotValid = false;

    // ⚠️ THE INVARIANT THIS EXISTS TO ENFORCE: ONE authoritative camera snapshot
    // per model generation, not six. `authoritativeSnapshots` counts writes to
    // the locked occurrence; `occurrenceDraws` counts every matching draw. Their
    // ratio is how many times the group draws per frame, which is the number run
    // thirty-four could not see.
    uint64_t occurrenceDraws = 0;
    uint64_t authoritativeSnapshots = 0;
    uint64_t occurrenceModelFrames = 0;
    bool     occurrenceLocked = false;
    uint32_t lockedOccurrence = 0;
};
CameraStats GetCameraStats ();

// MAIN THREAD, at teardown. Nothing here may outlive Archicad's device.
void ShutdownCamera ();

}   // namespace injection
}   // namespace dxgi
}   // namespace archviz
}   // namespace geomsrv

#endif
