#ifndef TAPIOCA_HUD_VIRTUAL_EDITOR_TERRAIN_HPP
#define TAPIOCA_HUD_VIRTUAL_EDITOR_TERRAIN_HPP

#include "Edit/EditLandscapeFieldPlan.hpp"
#include <optional>
#include "Edit/EditDocument.hpp"
#include <memory>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
struct Mesh;
}
namespace tapioca::geometry {
struct TerrainDesignOutcome;
}
namespace geomsrv::archviz::overlaylayers {
struct Layer;
}
namespace geomsrv::archviz::edithud {
struct Session;
struct TerrainDesignState;
struct TerrainCaptureRequest {
    edit::Id path = edit::kNoId; // kNoId: set the terrain alone; a design starts once a path exists.
    uint64_t revision = 0;
    std::string terrainGuid; // Empty: read exactly one native selected Mesh; otherwise recapture this source.
};
struct TerrainSectionDraft {
    edit::Id path = edit::kNoId, section = edit::kNoId;
    uint64_t revision = 0;
    edit::Section value;            // Shared dimensional values; mixed fields use `mixed` below.
    std::vector<edit::Id> sections; // Selected range IDs in authored order; one is the old API.
    // Bit positions in SectionField: set = apply, clear = mixed/preserve.
    // The UI sets a bit only when that field is edited; never a synthetic NaN.
    uint32_t mixed = 0;
};
enum class SectionField : uint32_t {
    WidthLeft = 1u << 0,
    WidthRight = 1u << 1,
    Taper = 1u << 2,
    EndLeft = 1u << 3,
    EndRight = 1u << 4,
    Crossfall = 1u << 5,
    CrossfallUnit = 1u << 6,
    Fall = 1u << 7,
    Shoulder = 1u << 8,
    Cut = 1u << 9,
    CutUnit = 1u << 10,
    Fill = 1u << 11,
    FillUnit = 1u << 12,
    Rounding = 1u << 13
};
uint32_t SectionFieldBit (SectionField field);
// Shared state and explicit selected ranges. Existing single-range calls wrap this.
bool BeginTerrainSectionEdit (Session& session, const std::vector<edit::Id>& sections);
bool BeginTerrainSectionEdit (Session& session, edit::Id section);
struct TerrainDesignBaseline {
    std::shared_ptr<const Mesh> mesh;
    bool nativeSolid = false; // Compact the upward surface on the worker.
    // Main-thread-only freshness read, never handed to a worker. False requires
    // an explicit recapture; the existing cache and any completion are hidden.
    std::function<bool ()> current;
};
struct TerrainDesignStatus {
    bool active = false, pending = false, current = false, built = false;
    bool stale = false; // The layer shows the last built surface; it no longer matches the document.
    uint64_t revision = 0;
    size_t vertices = 0, triangles = 0, conflicts = 0;
    double cut = 0, fill = 0;
    std::string error;
    // #175: the field preview. `previewPending`: one is being computed; `previewed`: the layer
    // shows it (the build has not caught up); `previewMs`: the last one's time on its worker.
    bool previewPending = false, previewed = false;
    double previewMs = 0;
    // The newest preview's cut/fill and shortfalls, in the build's words; whether a mesh build
    // was asked and has not started.
    double previewCut = 0, previewFill = 0;
    std::vector<std::string> previewUnmet;
    bool buildRequested = false;
};

// Detached plan-view terrain, captured by the host on the main thread. Starts
// one path design in the existing document/history; selection stays the same.
// The immutable baseline is retained across edits and surface switches. Closed
// native solids require an attributed surface adapter before this entrypoint.
bool StartTerrainDesign (Session& session, std::shared_ptr<const Mesh> mesh, edit::Id path);
bool StartTerrainDesign (Session& session, TerrainDesignBaseline baseline, edit::Id path);
// The terrain first (the user, 2026-10-24: "allow set terrain first"): with a design path the
// design starts at once; without one the baseline is held and PollTerrainDesign starts the
// design as soon as one can be resolved. No document edit until then.
bool SetTerrainBaseline (Session& session, TerrainDesignBaseline baseline, edit::Id path);
// The GUID of the terrain set, whether a design owns it yet or not; empty when none is.
std::string TerrainGuid (const Session& session);
TerrainDesignStatus GetTerrainDesignStatus (const Session& session);
// #191 U2: the command bar's build state, one of four (or none without a terrain design). A
// refusal stands until a newer build succeeds; Stale keeps its colour and says so in words (#71).
enum class ChipKind : uint8_t { None, Current, Building, Stale, Refused };
struct Chip {
    ChipKind kind = ChipKind::None;
    std::string text;   // what the chip says; a refusal only "Refused", which row 2 then explains
    std::string detail; // the whole of it, for its hover and the prompt row
};
Chip ChipState (const TerrainDesignStatus& status, const std::string& refusal);
// Keeps Session::buildRefusal from `status`: a refusal is taken as it comes; it is cleared only by
// a built, current outcome of a newer revision than the refused one.
void TrackBuildRefusal (Session& session, const TerrainDesignStatus& status);
// The design's current path request, as the terrain job will compile it (#105 recipe view);
// absent when no design exists, it grades regions only, or the build has no Rust kernel.
std::optional<tapioca::geometry::PathDesignRequest> TerrainRecipeRequest (const Session& session);
// Main-thread, nonblocking poll. One worker at a time; changed revisions coalesce
// until input ends. Results never enter document state or its undo history.
void PollTerrainDesign (Session& session);
// The status/diagnostic answer: the newest completed outcome that still describes
// the current document, whatever it built (#112 P4b). A refusal is reported here
// while the last good publication keeps drawing through LastPublishedTerrainDesign.
const tapioca::geometry::TerrainDesignOutcome* CurrentTerrainDesign (const Session& session);
// What the layer draws: the last GOOD publication of the same session and terrain
// (#112 P4b: a newer refusal, an in-flight rebuild or an edit never replace it),
// with `stale` set when it predates the current request. Null when there is none
// or the terrain capture is invalid. Display only; status reads CurrentTerrainDesign.
const tapioca::geometry::TerrainDesignOutcome* LastPublishedTerrainDesign (const Session& session, bool& stale);
void DrawTerrainDesign (const Session& session, overlaylayers::Layer& layer);
// #149 T4: the located outline-step witnesses of a built outcome as one marker set per kind --
// faces ending at the reach limit, faces stepping at the region's edge, and the mesher's step
// strips -- in the colours OutlineStepLegend names. Nothing for an unbuilt outcome.
void AppendOutlineStepPlaces (const tapioca::geometry::TerrainDesignOutcome& outcome, overlaylayers::Layer& layer);
// One legend line per marked kind, "N of M" where the build counted M observations.
std::vector<std::string> OutlineStepLegend (const tapioca::geometry::TerrainDesignOutcome& outcome);
// The marker colour of each kind (RGBA), shared by the markers and the legend swatch.
constexpr uint32_t kOutlineReachRgba = 0xEAB308FFu, kOutlineRingRgba = 0xF97316FFu, kOutlineStripRgba = 0xA855F7FFu;
void TerrainDesignPanel (Session& session);
// #175: while the terrain is being regenerated, a line saying so at the top of the editor's
// panel and a small badge at the top of the view (the foreground draw list: no window, so it
// takes no hover and no cursor claim). Nothing while it is current.
void TerrainActivityBadge (const Session& session);
// #175: build the mesh of the current revision (what an Archicad Mesh is built from) on the
// worker, once input is idle. False, and a message, when it is built or cannot be.
bool RequestTerrainBuild (Session& session);
// #175: the panel's Build mesh button and, while the preview stands in, its quantities and
// shortfalls.
void TerrainBuildSection (Session& session);
// Grade along the chosen path's band only (true), or from every path's paved outline (false, the
// default); a running design rebuilds the other way on the next idle poll.
void SetTerrainBand (Session& session, bool band);
bool SetTerrainPaths (Session& session, bool enabled);
// Rebuild and draw the terrain, or leave the paths alone to draw and edit (Session::terrainLive).
void SetTerrainLive (Session& session, bool live);
// Setup uses only live authored centreline paths, never native/model GUIDs as
// virtual path IDs. Initial choice follows one selected path or the sole eligible
// path; an unavailable explicit choice is not silently retargeted.
std::vector<edit::Id> TerrainDesignPaths (const Session& session);
edit::Id ResolveTerrainDesignPath (Session& session);
bool ChooseTerrainDesignPath (Session& session, edit::Id path);
bool EditTerrainDesignPath (Session& session);
bool DrawTerrainDesignPath (Session& session);
bool DrawTerrainPropertyLine (Session& session);
void TerrainSetupPanel (Session& session);
// One explicit Apply through the existing Session history. Drafts are revision-
// bound; stale/missing ranges refuse rather than retarget or overwrite newer edits.
bool BeginTerrainSectionEdit (Session& session, edit::Id section);
bool ApplyTerrainSectionEdit (Session& session);
// A cross-section slider held, moved and released: the draft applies live within one editor
// gesture, the terrain preview rebuilding as it goes, and the release records one undo step.
bool BeginTerrainSectionDrag (Session& session);
bool MoveTerrainSectionDrag (Session& session);
void EndTerrainSectionDrag (Session& session);
// #24 Esc: a held cross-section slider's gesture restores the document and the typed values as
// they were before it; true when there was one. Nothing more applies until the release.
bool CancelTerrainSectionDrag (Session& session);
// Selected guide vertices become point controls; selected whole open polylines
// become line controls. One existing Session history entry for the whole set.
bool SetTerrainControlSelection (Session& session, bool enabled);
// Pure UI request/ownership gate, no native read or document/history mutation.
bool RequestTerrainCapture (Session& session, bool fromSelection);
bool TerrainCaptureIsCurrent (const Session& session, const std::shared_ptr<const TerrainCaptureRequest>& request);
} // namespace geomsrv::archviz::edithud
#endif
