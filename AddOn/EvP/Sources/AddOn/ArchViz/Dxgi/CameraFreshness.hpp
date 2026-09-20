// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// ArchViz/Dxgi/CameraFreshness -- how old is the camera this Present draws with,
// and does it still belong to the window on screen? TWO DIFFERENT FAULTS.
//
// ⚠️ ORDINARY LATENESS IS NOT STALENESS AND MUST NOT BE
// TREATED AS IT. During navigation the selected draw sometimes has not happened
// yet in the frame being presented, so the overlay composes with the previous
// frame's camera and slips by one frame against the building. That is measured
// here and DELIBERATELY NOT SUPPRESSED: blanking those frames would trade a
// sub-pixel slip for visible flicker. `GhostMesh` binds no camera of its own and
// is displaced by exactly the same amount -- it simply has nothing behind it to
// line up against, which is why it looked healthier.
//
// ⚠️ A RESIZE IS THE OTHER FAULT AND HAS NO CLOCK. Scaling a
// window changes the swap chain WITHOUT Archicad redrawing its model, so no new
// camera is ever produced and the old projection persists until the user happens
// to orbit. A run read `vp=2176x1350 target=1221x987 LATE+15`: fifteen
// consecutive Presents drawing the building beside itself. That IS suppressed --
// one blank frame beats the building drawn twice -- and it raises a request for
// the redraw that can end it.
//
// THREAD SAFETY. `NoteTargetExtent`, `NoteAccepted`, `Stale`, `NoteSuppressed`
// and `NoteAge` run on Archicad's render thread inside Present: atomics only, no
// lock, no allocation, no ACAPI. `TakeRedrawRequest`, `Snapshot` and `Reset` are
// for the main thread.

#ifndef GEOMSRV_ARCHVIZ_DXGI_CAMERAFRESHNESS_HPP
#define GEOMSRV_ARCHVIZ_DXGI_CAMERAFRESHNESS_HPP

#include <cstdint>

struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace injection {
namespace freshness {

// RENDER THREAD, first thing in Present. One DXGI call, no buffer fetched.
void NoteTargetExtent (IDXGISwapChain* swapChain);

// RENDER THREAD, when a camera snapshot is accepted: remember the window size it
// was measured for.
void NoteAccepted ();

// ANY THREAD, PURE. True when the accepted camera belongs to a window size that
// no longer exists.
//
// ⚠️ THE TWO EXTENTS ARE THE SAME QUANTITY MEASURED AT TWO
// TIMES, so this needs no tolerance -- unlike comparing the 3D view's viewport
// against the back buffer, which is legitimately a pixel narrower.
bool Stale ();

// RENDER THREAD, when a Present was refused because `Stale`. Counts it and
// raises the coalesced redraw request: only Archicad can end this, and only by
// redrawing its model.
void NoteSuppressed ();

// RENDER THREAD, once per Present that had a camera to judge.
void NoteAge (uint64_t presentGeneration, uint64_t snapshotGeneration);

// ANY THREAD. The window extent the composition is currently trying to reach,
// packed width << 16 | height, or 0 before the first Present.
//
// ⚠️ THIS IS THE RETRY EPOCH AND MODEL FRAMES ARE NOT.
// Bounding retries by "model generations advanced" is wrong: a redraw that
// advances the generation WITHOUT producing a camera accepted for this extent
// would reset the budget and buy another attempt, and a window that keeps
// redrawing for unrelated reasons would buy attempts forever. The attempt budget
// belongs to the extent being repaired, so two failures for THIS extent are two
// failures full stop.
uint32_t TargetEpoch ();

// ANY THREAD. The window extent the composition is currently trying to reach,
// packed width << 16 | height, or 0 before the first Present.
//
// ⚠️ THIS IS THE RETRY EPOCH AND MODEL FRAMES ARE NOT.
// Bounding retries by "model generations advanced" is wrong: a redraw that
// advances the generation WITHOUT producing a camera accepted for this extent
// would reset the budget and buy another attempt, and a window that keeps
// redrawing for unrelated reasons would buy attempts forever. The attempt budget
// belongs to the extent being repaired, so two failures for THIS extent are two
// failures full stop.

// MAIN THREAD. True once per raised request -- COALESCED, so a hundred suppressed
// Presents produce one. The caller decides whether it is allowed to act on it.
//
// ⚠️ HOW LONG IT WAITED IS MEASURED, BECAUSE ONLY ONE
// THING CONSUMES IT AND THAT THING IS A `WM_TIMER`. `OverlayRedrawBudget::
// Consider` is the sole caller, it runs from the injected runtime's 250 ms
// heartbeat, and `CameraWake.hpp` records what a `WM_TIMER` does under a drag:
// "synthesised only when the queue has nothing else in it ... during a drag
// Archicad's queue is never empty, so the timer is served last however short its
// interval", measured at 24-41 ms for a 15 ms request in 2026-08-13. A request
// raised by a SUPPRESSED Present -- the overlay is off the screen right now --
// waiting on the lowest-priority message Windows has is the shape of
// OVERLAY-INVARIANTS.md section 9, and the wait is the number that says so.
bool TakeRedrawRequest ();

// RENDER THREAD, once per Present the renderer classified REPEAT_SCENE.
//
// ⚠️ REPEAT_SCENE MAKES A CLAIM ABOUT ARCHICAD AND NOTHING
// TESTED IT. It asserts Archicad "presented without re-rendering the model -- a
// UI repaint, a palette, a cursor -- so the geometry on screen is the geometry
// this camera drew", and on the strength of that the renderer DISCARDS the fresh
// camera and draws with the previous one. That is sound if and only if
// `modelSceneGeneration` advances on every model pass. If it ever misses one,
// the overlay is drawn with the previous frame's transform over a building that
// has moved -- which from outside is the overlay lagging, or, at orbit speed,
// the overlay not being there.
//
// ⚠️ AND NO EXISTING COUNTER CAN SEE IT, INCLUDING THE
// ONE THAT LOOKS LIKE IT SHOULD. `age0` compares the present generation against
// the snapshot generation; a generation that did not advance makes both the same
// stale number and the age reads 0 -- perfectly fresh, by a measure that is
// asking the wrong question. The 2026-09-19 menu run read `age0 99.5%` and
// `cam(new+6 repeat+22)` on the frames the overlay was reported missing from.
//
// So the claim is partitioned, per event, by what the newest snapshot says at
// the instant the decision is taken:
//
//   held        same scene pass, same camera window -- the claim is TRUE
//   passMoved   a LATER scene pass has been snapshotted: Archicad DID re-render
//   windowMoved same pass, but `b1`/`b2` point at a different window of the
//               constant ring, which finding 3 calls the camera's address
//
// `passMoved` above zero during navigation falsifies REPEAT_SCENE outright.
// `held` alone leaves the classification correct and sends the question on to
// the composite output. `usable` false means there was no snapshot to judge
// against and the Present is counted in none of the three.
void NoteRepeatScene (bool usable, bool passMoved, bool windowMoved);

// ---------------------------------------------------------------------------
// CAMERA CONTENT, which is a different question from every other one in this
// file and from every counter in the tree.
//
// ⚠️ `repeat(held N, PASS MOVED 0, window moved 0)` DOES
// NOT MEAN THE CAMERA DID NOT MOVE. It means the scene-pass generation and the
// constant-buffer window identity did not move. A D3D11 constant buffer can be
// REWRITTEN IN PLACE at the same resource and the same offset, and the 2026-09-19
// fast-pan reproduction is what that looks like from outside: Archicad's model
// reaches the new view, the overlay stays rendered at the previous camera, and it
// stays there after the pan ends until some other interaction repairs it. 2353
// `held` frames were recorded across that session. `held` was measuring identity
// and the question was content.
//
// ⚠️ SO THE SIGNATURE IS TAKEN FROM DECODED VALUES,
// NOT FROM BINDINGS, AND IT COSTS NOTHING BECAUSE THE DECODE ALREADY HAPPENS.
// `CameraCensus::TryResolve` maps the staged copy with
// `D3D11_MAP_FLAG_DO_NOT_WAIT` and already holds `view[16]`, `projection[16]` and
// the viewport on the CPU in order to score the variants. `NoteCameraContent` is
// called from exactly there, for the SELECTED group only. No new readback, no
// `Map` at Present, no synchronisation -- InjectionCamera.hpp's rule that the
// composition path copies GPU-to-GPU stands untouched.
//
// The hash is over the raw bit patterns. There is no floating-point noise to
// quantise away: the bytes are copied verbatim out of Archicad's ring, so equal
// content is bit-equal content.
void NoteCameraContent (const float* view16, const float* projection16, float vpX, float vpY, float vpW, float vpH);

// ANY THREAD. Latest asynchronously decoded selected-camera content. This is a
// correlation value for diagnostics, not the proof of pass equality.
uint64_t ContentSignature ();

// RENDER THREAD, when the renderer takes a fresh camera for composition
// (`NEW_SCENE`). Stamps the accepted camera with the content signature and the
// capture serial that were current at that instant.
void NoteCameraAdopted ();

// RENDER THREAD, from `CopyCameraWindows`, when the 256 bytes the shader reads
// are actually refreshed.
//
// ⚠️ THIS IS THE ONE THAT DECIDES WHAT IS ON SCREEN, AND
// IT IS NOT THE ADOPTION CLASSIFICATION. `NEW_SCENE` / `REPEAT_SCENE` chooses
// what goes into `g_acceptedCamera`, which is METADATA -- a viewport, a
// generation, a draw sequence. The projection the shader actually multiplies by
// lives in the snapshot buffers, and `CopyCameraWindows` overwrites those on
// EVERY QUALIFYING DRAW regardless of how the Present was classified.
//
// ⚠️ SO `FRESH_NOT_ADOPTED` DOES NOT PROVE THE OVERLAY
// DREW WITH A STALE CAMERA. It proves the accepted METADATA was stale. The
// bytes are gated further up, by the chain
//
//     census draw -> MatchesSelection (the PIN) -> SnapshotSelectedDraw
//                 -> occurrence locked -> CopyCameraWindows
//
// and any link failing stops the refresh with nothing downstream reporting it.
// A freeze is therefore visible here and nowhere else: during one, this stops
// being called while the census keeps decoding fresh content, because the census
// matches a GROUP and this needs the PIN.
void NoteAuthoritativeSnapshot ();

// ⚠️ AND EVERY PRESENT IS CLASSIFIED AGAINST THAT
// STAMP, WHICH IS THE WHOLE INSTRUMENT:
//
//   ADOPTED            this Present took a fresh camera
//   SAME               latest content signature == the accepted one; the overlay
//                      is drawing what Archicad is drawing
//   FRESH_NOT_ADOPTED  the signature has MOVED and the overlay is still composing
//                      with the camera it accepted earlier
//
// `FRESH_NOT_ADOPTED` above zero during a visible freeze is the adoption bug,
// stated as a measurement. A signature that never changes while Archicad visibly
// pans is a CAPTURE bug instead, and sends the next instrument to the buffer
// write path. The two are distinguishable only because the content is signed.
//
// ⚠️ KEPT DELIBERATELY APART FROM
// `modelSceneGeneration`, `scenePassGeneration` AND THE WINDOW OFFSET. Those are
// useful metadata and they are recorded beside this, but none of them is a
// substitute for camera equality -- believing that one of them was is what cost
// the previous run.

// ---------------------------------------------------------------------------
// The matrix ledger
// ---------------------------------------------------------------------------
//
// ⚠️ EVERY MEASUREMENT SO FAR HAS BEEN INDIRECT. Centre
// error, spread, inside-clip and the variant scores all describe what a matrix
// DOES to an approximate anchor. None of them says what the matrix IS, and the
// question now on the table -- do the four draws of a model pass bind four
// different transforms, or one matrix written four times -- is answered by
// sixteen floats and not by a score.
//
// ⚠️ THE CENSUS ALREADY HAS THEM AS VALUES. Its readback
// maps the staged view and projection windows to score the variants, and its
// own comment calls that "THE ONE PLACE ARCHICAD'S CAMERA EXISTS AS VALUES
// RATHER THAN AS A BINDING". This keeps a copy of the first decode of each
// group so two groups can be compared side by side; there is no second readback
// and nothing new is mapped.
//
// ⚠️ AND THE GENERATION TRAVELS WITH THEM, because two
// matrices read in different model frames are not comparable -- the camera
// moved between them. Only rows sharing a generation answer the question.

struct GroupMatrices {
    uint32_t groupId = 0;
    uint32_t occurrence = 0;
    uint64_t generation = 0;
    float view[16] = {};
    float projection[16] = {};
};

// RENDER THREAD, from the census readback. Keeps the FIRST decode per group.
void NoteGroupMatrices (uint32_t groupId, uint32_t occurrence, uint64_t generation, const float view[16],
                        const float projection[16]);

// MAIN THREAD. Copies out up to `capacity` entries; returns how many.
size_t GetGroupMatrices (GroupMatrices* out, size_t capacity);

// MAIN THREAD. How many are held, so a reporter can print each one once.
size_t GroupMatrixCount ();

// ---------------------------------------------------------------------------
// The scene epoch gate
// ---------------------------------------------------------------------------
//
// ⚠️ THE QUESTION IT ANSWERS. The overlay captures its camera
// inside Archicad's scene pass and composites at Present. Those are different
// points in the frame, so the camera it draws with is only correct if no newer
// scene pass has been produced in between -- and NOTHING measured that. Every
// composition counter stays healthy while the overlay draws a correct picture
// of the wrong moment, which from outside is exactly "it lags behind".
//
// ⚠️ AND IT IS A STRUCTURAL COMPARISON, NOT A TIMING ONE.
// The camera is stamped with the generation of the scene pass it was READ FROM;
// the Present is stamped with the generation of the scene pass that produced
// the image being posted. Equal means the camera, the depth buffer and the
// pixels all came from one pass. "Recent enough" would let unrelated timing
// decide correctness, which is the fault, not the test.
//
// ⚠️ COUNTING IS ALWAYS ON; ENFORCING IS OFF BY DEFAULT
// (section 10). With the gate off this changes nothing that is drawn and still
// reports how often the overlay WOULD have been suppressed, so the measurement
// costs no run. With it on, a mismatch skips the composite -- the overlay then
// flickers at the model's own rate instead of lagging, which is the visible
// form of the same answer.

// RENDER THREAD, from `CopyCameraWindows`: the camera bytes just copied were
// read during this scene-pass generation.
void NoteCameraEpoch (uint64_t sceneGeneration);

// RENDER THREAD, from `InjectAtPresent`: the image about to be presented was
// produced by this scene-pass generation. Returns whether the composite may
// proceed -- always true while the gate is off. Counts either way.
bool EpochGateAllows (uint64_t presentedGeneration);

// MAIN THREAD. Off by default, and reset at every arm.
void SetEpochGate (bool enabled);
bool EpochGate ();

struct EpochGateReport {
    uint64_t matched = 0;    // camera and image from one pass
    uint64_t mismatched = 0; // they were not
    uint64_t suppressed = 0; // ... and the gate was on, so nothing was drawn
    uint64_t behindMax = 0;  // worst generations the camera was BEHIND the image
    uint64_t aheadMax = 0;   // worst it was AHEAD -- a different fault entirely
    uint64_t cameraEpoch = 0;
    uint64_t presentedEpoch = 0;
    bool enabled = false;
};
EpochGateReport GetEpochGate ();

struct Report {
    // ⚠️ BUCKETS, NOT A MEAN. A mean lets one forty-frame
    // stall hide a thousand good frames, and the question is what the STEADY
    // STATE does. `age0` is the overlay exactly on the building.
    uint64_t age0 = 0;
    uint64_t age1 = 0;
    uint64_t age2 = 0;
    uint64_t age3plus = 0;
    uint32_t ageMax = 0;
    uint64_t samples = 0;
    uint64_t suppressed = 0;

    // ⚠️ THE GAP BETWEEN THE OVERLAY GOING BLANK AND
    // ANYONE ASKING FOR THE FRAME THAT ENDS IT. `NoteSuppressed` raises the
    // request on the render thread the moment a Present draws nothing;
    // `TakeRedrawRequest` is reached only from the main-thread heartbeat. This is
    // the milliseconds between the two, worst case, and `redrawsTaken` is how
    // many requests ever got that far -- a request raised and never taken is an
    // overlay that stayed blank.
    uint32_t redrawWaitMaxMs = 0;
    uint64_t redrawsTaken = 0;

    // See `NoteRepeatScene`. These three partition every REPEAT_SCENE Present
    // that had a snapshot to be judged against.
    uint64_t repeatHeld = 0;
    uint64_t repeatPassMoved = 0;
    uint64_t repeatWindowMoved = 0;

    // ---- camera CONTENT, see `NoteCameraContent` --------------------------
    uint64_t camAdopted = 0;
    uint64_t camSame = 0;
    uint64_t camFreshNotAdopted = 0;
    // Captures Archicad has produced, and decodes that reached the CPU. A
    // `contentDecodes` that stops climbing while the view visibly moves is a
    // capture fault and not an adoption fault.
    uint64_t captureSerial = 0;
    uint64_t contentDecodes = 0;
    uint64_t contentChanges = 0;
    // How far behind the accepted camera fell, at its worst.
    uint64_t serialLagMax = 0;
    uint32_t msSinceLatestCaptureMax = 0;
    uint32_t msSinceAcceptedChangedMax = 0;
    // ⚠️ THE LONGEST UNBROKEN RUN, BECAUSE A TOTAL
    // CANNOT DESCRIBE A FREEZE. Six hundred scattered FRESH_NOT_ADOPTED frames
    // are a wobble; six hundred consecutive ones are the screenshot.
    uint64_t freshRunMax = 0;
    uint64_t freshRunCurrent = 0;

    // ⚠️ WHAT CHANGED AT THE MOMENT THE OVERLAY
    // RECOVERED. The reproduction says the freeze persists until "another
    // interaction" repairs it, so the repair is the evidence: these describe the
    // longest run that ENDED, and which of the candidate causes moved when it did.
    // ⚠️ WHEN THE BYTES LAST MOVED, WHICH IS THE
    // ONLY DIRECT MEASURE OF WHAT IS ON SCREEN. `msSinceSnapshotMax` in the
    // seconds, against a `contentChanges` that kept climbing, is the pin
    // dropping out -- the overlay holding the last camera it was given while
    // Archicad went somewhere else.
    uint64_t snapshots = 0;
    uint32_t msSinceSnapshot = 0;
    uint32_t msSinceSnapshotMax = 0;

    uint64_t recoveryRunLength = 0;
    uint32_t recoveryMs = 0;
    bool recoverySignatureChanged = false;
    bool recoveryPassMoved = false;
    bool recoveryWindowMoved = false;
};
Report Snapshot ();

// MAIN THREAD, at the start of an overlay session.
void Reset ();

} // namespace freshness
} // namespace injection
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
