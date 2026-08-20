#ifndef EVP_ARCHVIZ_SCENECMDQUEUE_HPP
#define EVP_ARCHVIZ_SCENECMDQUEUE_HPP

// Geometry, main/extraction thread -> render thread (plan §6.2).
//
// ⚠️ OWNERSHIP IS THE WHOLE POINT OF THIS FILE. The producer heap-allocates a
// payload and hands it over; the consumer owns it from that moment and frees it
// once the GPU buffer is made. It is a queue of `unique_ptr` nodes and NOT a POD
// union with pointers into main-thread memory, because the extractor reuses its
// buffers between elements — a raw pointer here would be read by the render
// thread after the extractor had already overwritten it. That is the same shape
// as `bgfx::makeRef` on a local, which the sandbox paid for once and which
// presented as a SHADING bug (plan §1.1). Nothing here can be handed a pointer
// it does not own.
//
// ⚠️ NO bgfx, NO bx, NO ACAPI — deliberately, so `tests/cpp` can compile the
// real source. The queue is where a lost or double-freed element would hide, and
// that is exactly the class of bug a debugger inside Archicad is worst at.
// Everything GPU-shaped lives in SceneCache, on the other side of the handover.
//
// ⚠️ ONLY WHAT PHASE 6 NEEDS IS HERE. §6.2 sketches SetTransform, UpsertTexture,
// SetStoreyLines and SetSelection; those arrive with the phase that acts on
// them. In particular §6.2's own warning stands: `SetTransform` is only
// meaningful if geometry is in LOCAL space, and `geomsrv::ExtractElement`
// produces WORLD-space metres — so a transform command would be unimplementable
// against the extractor we are reusing. A command that exists and cannot work is
// worse than one that does not exist.

#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/MeshGroups.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {

enum class SceneCmdType : uint8_t {
    // A rebuild is starting. The render thread may show "updating" and, on
    // EndBatch, drop any element it did not hear about during a FULL batch.
    BeginBatch,
    UpsertElement,
    RemoveElement,
    EndBatch,
    // The model's surface pool, whole, replacing whatever the consumer held.
    // ⚠️ IT MUST PRECEDE THE UPSERTS OF ITS BATCH. The pool is renumbered
    // whenever the model is rebuilt, so an element drawn against the PREVIOUS
    // table is drawn in another surface's colours — plausible-looking and wrong,
    // which is the failure mode this whole file is written against. The producer
    // pushes it immediately after BeginBatch; ordering is guaranteed by Take's
    // erase-the-front (see the .cpp).
    SetMaterials,
    // Sun direction and ambient level, from Archicad's own place settings.
    SetEnvironment,
    // Which elements are selected, WHOLE — not a delta.
    //
    // ⚠️ A SET, NOT add/remove, because Archicad's selection is a set and the
    // bridge that mirrors it polls rather than subscribes. A delta protocol over
    // a poll would need the two sides to agree on a baseline they have no way to
    // establish, and would drift silently the first time a poll was missed.
    // Costs one small vector per change; the selection is tens of elements.
    SetSelection,
};

// One element's geometry, ready for the GPU: nothing here needs interpreting,
// converting or re-ordering on the render thread.
struct ElementUpload {
    std::string guid;

    // ⚠️ FLOAT, CONVERTED BY THE PRODUCER. `geomsrv::Mesh::vertices` is DOUBLE
    // (world metres); the GPU wants float, and the conversion belongs on the
    // producing side so the render thread never does per-vertex work.
    //
    // ⚠️ AND FLOAT IS NOT ENOUGH FOR A GEOREFERENCED PROJECT. A survey-placed
    // model can sit at coordinates in the millions, where float32 has ~10 cm of
    // resolution — geometry then visibly quantises and z-fights, and it looks
    // like bad tessellation rather than lost precision. `bounds` is carried so a
    // later rebase (subtract the snapshot centroid, put it in the view matrix)
    // is possible without re-extracting. NOT done speculatively: measure a real
    // georeferenced project first. Tracked as PLAT-BGFX-P6-PRECISION.
    std::vector<float>    vertices;   // xyz interleaved, world metres, Z-up
    std::vector<float>    normals;    // xyz per vertex, Archicad's true per-corner normals

    // ⚠️ ALREADY MATERIAL-GROUPED by MeshGroups::BuildMaterialGroups, and the
    // ranges match. Doing it here rather than on the render thread keeps a
    // per-triangle sort off the frame loop, and keeps the one algorithm that has
    // an offline test on the side of the seam the test can reach.
    std::vector<uint32_t>      indices;
    std::vector<MaterialRange> ranges;

    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };

    size_t VertexCount () const { return vertices.size () / 3; }
    // Retained heap bytes, so "how much is the viewer holding" is reportable
    // rather than folklore — the same courtesy geomsrv::Mesh::Bytes offers.
    size_t Bytes () const;
};

// Sun and ambient, as the renderer wants them. Filled from
// ACAPI_GeoLocation_GetPlaceSets + CalcSunOnPlace by the producer.
//
// ⚠️ ARCHICAD COMPUTES THE SUN. Do NOT implement a solar-position model here or
// anywhere else in the add-on (plan §3): a viewer whose sun disagrees with the
// shadows Archicad renders is worse than one with a fixed light. The convention
// — sunAngXY is CCW from +X in MODEL space, sunAngZ is altitude above the
// horizon — was settled against an independent NOAA calculation and lives in
// NativeCommands/ProjectCommands.cpp → GetPlaceInfoCommand. This struct carries
// the RESULT of that conversion, so the two cannot drift into two conventions.
struct EnvironmentUpload {
    // Direction TOWARD the sun, world space, Z-up. Need not be normalised — the
    // consumer normalises, because a zero-length vector from a sun below the
    // horizon must not become a NaN in a shader.
    float sunX = 0.0f, sunY = 0.0f, sunZ = 1.0f;
    // What a surface facing away from the sun still receives, 0..1.
    float ambient = 0.35f;
    // Below the horizon: the producer still sends the vector (it is where the
    // sun IS), and the consumer may choose to light the model as overcast rather
    // than from underneath. Reported rather than silently clamped.
    bool  sunBelowHorizon = false;
    // Project north as the SAME kind of angle as everything else here:
    // counterclockwise from +X, degrees. Archicad's default is 90, which puts
    // north along +Y.
    //
    // ⚠️ IT IS CARRIED FOR REPORTING, NOT FOR THE SUN VECTOR. The vector above
    // is already in model space and needs no north term (settled against NOAA in
    // NativeCommands/ProjectCommands.cpp -> GetPlaceInfo). North is here so the
    // HUD can also show the COMPASS BEARING, which is the number Archicad's own
    // dialogs display and the only one a user can compare against.
    float northDegrees = 90.0f;
    // The bearing itself, precomputed by the producer. ⚠️ ONE DERIVATION: the
    // consumer used to recompute `north - azimuth` from the two angles it was
    // given, and two copies of the formula whose two readings ARE the history of
    // this feature is exactly the pair that drifts.
    float bearingDegrees = 0.0f;

    // ---- WHERE THE SUN CAME FROM, so the HUD can say it ---------------------
    // ⚠️ THE ANGLES ABOVE ARE ARCHICAD'S *STORED* ONES, NOT RECOMPUTED FROM THIS
    // DATE. A user who typed an azimuth and altitude into the Sun dialog has a
    // stored sun the calendar does not imply, and recomputing silently replaces
    // their sun with a different one -- see ExtractionThread::ReadEnvironment.
    // These fields exist so the two can be COMPARED rather than one being
    // preferred behind the user's back.
    float latitudeDegrees = 0.0f;
    float longitudeDegrees = 0.0f;
    float altitudeMetres = 0.0f;
    uint16_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool summerTime = false;
    int16_t timeZoneMinutes = 0;
    // What ACAPI_GeoLocation_CalcSunOnPlace makes of that date and place. Carried
    // for COMPARISON only; nothing lights the scene with it.
    bool haveComputedSun = false;
    float computedAzimuthDegrees = 0.0f;
    float computedAltitudeDegrees = 0.0f;
};

struct SceneCmd {
    SceneCmdType                   type = SceneCmdType::BeginBatch;
    // Set for UpsertElement, null otherwise.
    std::unique_ptr<ElementUpload> upload;
    // Set for SetMaterials, null otherwise. Owning, same handover rule as
    // `upload`: the producer builds it and never touches it again.
    std::unique_ptr<MaterialTable> materials;
    // SetEnvironment only. By value — it is four floats, and a heap node for
    // that would be ceremony.
    EnvironmentUpload              environment;
    // Set for RemoveElement.
    std::string                    guid;
    // SetSelection only: the whole selected set, in Archicad's GUID string form.
    std::vector<std::string>       selection;
    // BeginBatch only: a FULL batch replaces the scene, so the consumer may drop
    // anything not mentioned before EndBatch. A partial batch touches only the
    // elements it names. Getting this backwards on a partial refresh deletes the
    // building and leaves the twelve walls that changed.
    bool                           full = false;
};

// Unbounded, mutex-guarded, single-producer/single-consumer by convention.
//
// A mutex rather than a lock-free ring, for the same reason as
// InputRingBuffer: it is held for a pointer move, and a hand-rolled lock-free
// queue would buy nothing measurable while costing a class of bug that is very
// hard to see. Revisit only with a measurement.
//
// ⚠️ UNBOUNDED IS A DECISION WITH A COST. A full extraction of a large project
// can queue thousands of elements faster than the render thread uploads them, so
// peak memory is roughly the whole snapshot twice. `PendingBytes ()` exists so
// the producer can throttle itself; §6.5's slice budget is where that gets
// tuned, against a measurement rather than a guess.
class SceneCmdQueue final {
public:
    static SceneCmdQueue& Get ();

    // ---- producer ----
    void PushBeginBatch (bool full);
    void PushUpsert (std::unique_ptr<ElementUpload> upload);
    void PushRemove (const std::string& guid);
    void PushEndBatch ();
    void PushMaterials (std::unique_ptr<MaterialTable> materials);
    void PushEnvironment (const EnvironmentUpload& environment);
    void PushSelection (std::vector<std::string> guids);

    // ---- consumer, on the render thread ----
    // Move out up to `max` commands. Bounded on purpose: draining an entire
    // rebuild inside one frame is how a viewer stops presenting for a second and
    // takes Archicad's UI thread's frame budget with it, which plan §0's hard
    // requirement forbids. Returns what it took; empty when there is nothing.
    std::vector<SceneCmd> Take (size_t max);

    // Drop everything undelivered. For teardown, and for a full rebuild that
    // supersedes a queued partial one.
    void Clear ();

    size_t PendingCount () const;
    size_t PendingBytes () const;

private:
    SceneCmdQueue () = default;
    SceneCmdQueue (const SceneCmdQueue&) = delete;
    SceneCmdQueue& operator= (const SceneCmdQueue&) = delete;

    mutable std::mutex    mutex_;
    std::vector<SceneCmd> queue_;
    size_t                pendingBytes_ = 0;
};

}   // namespace archviz
}   // namespace geomsrv

#endif
