#ifndef TAPIOCA_EDIT_PUBLISHEDTERRAIN_HPP
#define TAPIOCA_EDIT_PUBLISHEDTERRAIN_HPP

// #112 P4a: one neutral immutable published terrain. Both build authorities
// (the unified field build and, during migration, the legacy path lane) convert
// to this type at the publication boundary; preview, quantities, debug replay
// and status consume it instead of a lane-specific result. It owns the full
// input provenance (domains=7) plus the mesh-ready domains=5 projection, and it
// carries the derived rebuild scope separately from the authored
// property/measurement scope: displayed site/context totals keep the existing
// 0.25 m measured-grid convention against the measurement region, while the
// authoritative earthwork quantities are triangle-exact and independent of any
// sampling resolution. #112 P4b: the mixed production lane publishes this
// snapshot on the outcome; the publisher keeps it as the last-good drawable.

#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditLandscapeUnifiedBuild.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include "Geometry/GeometryRustUnifiedTerrain.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tapioca::geometry {

// Ordered stack identity behind one `owners` value: the entry or path whose
// composition last moved the vertex. Indexed by the `owners` value.
struct PublishedOwner {
    uint32_t order = 0; // Stack position; paths follow every region entry.
    OperationId operation = 0;
    ObjectId object = 0;
    bool path = false;
    bool operator== (const PublishedOwner&) const = default;
};

// Authoritative triangle-exact quantities of one domain. `order` is UINT32_MAX
// for the reconciling unattributed (baseline) row: triangles no domain claims
// and no owner moved. Domain rows plus that row sum exactly to the total.
struct PublishedDomainQuantity {
    uint32_t order = 0;
    OperationId operation = 0;
    ObjectId object = 0;
    double cut = 0, fill = 0, net = 0, area = 0; // Metres/metres^2; net = fill - cut.
    uint64_t triangles = 0;                      // Attributed triangles (before zero-contour split).
    bool operator== (const PublishedDomainQuantity&) const = default;
};

// One locked vertex with its authored control reference.
struct PublishedLocked {
    uint32_t vertex = 0;
    double height = 0; // Metres; the exact height is also in `points`.
    uint64_t source = 0, authoredVertex = 0;
    bool point = false;
    bool operator== (const PublishedLocked&) const = default;
};

struct PublishedTerrain {
    // Geometry: proposed xyz, sampled baseline height per vertex, triangles.
    std::vector<double> points;
    std::vector<double> base;
    std::vector<uint32_t> triangles;
    // Ordered attribution: `owners[i]` indexes `ownersIdentity`, nullopt keeps
    // the baseline. Mappings and line origins are the unified support rows.
    std::vector<std::optional<uint32_t>> owners;
    std::vector<PublishedOwner> ownersIdentity;
    std::vector<TerrainMapping> mappings;
    std::vector<UnifiedLine> lines;
    std::vector<PublishedLocked> locked;
    // Full domains=7 input catalog, retained unchanged, and the domains=5
    // mesh-ready projection (native baseline prefix preserved) for consumers
    // that only need vertex/face attribution.
    ProvenanceRequest inputProvenance;
    ProvenanceRequest meshProvenance;
    // Rebuild scope versus authored measurement scope, kept explicitly separate.
    std::vector<double> rebuildRegion;     // Geometry replaced by this build (derived).
    std::vector<double> measurementRegion; // Authored property or validated cover.
    // Authoritative triangle-exact total over the whole published surface, the
    // per-domain partition (domain rows plus the UINT32_MAX baseline row sum
    // exactly to it), the legacy-display 0.25 m totals against the measurement
    // region, and the kernel-exact site/context against the rebuild region.
    // The 0.25 m numbers are a visualization/validation convention, never the
    // authority; the kernel-exact numbers are rebuild-scope, not site totals.
    HybridVolumes quantities;
    std::vector<PublishedDomainQuantity> domainQuantities;
    HybridVolumes displaySite, displayContext;
    HybridVolumes exactSite, exactContext;
    // Identity.
    uint64_t revision = 0, planHash = 0;
    std::array<uint8_t, 16> session {};
    std::string sourceTerrainGuid;
    bool operator== (const PublishedTerrain&) const = default;
};

// Unified lane: convert one successful `BuildUnifiedLandscapeTerrain` result
// into the neutral published terrain. `plan` supplies ordered owner identity,
// domain footprints and authored control references; `baseline` is the capture
// the build ran against (display totals measure baseline versus proposed at
// the existing 0.25 m convention); `scope` is the adapter-reported rebuild and
// measurement scope. Fails closed on shape/attribution mismatch; `published`
// is untouched on refusal.
bool PublishUnifiedTerrain (const UnifiedTerrainResult& unified, const FieldPlan& plan, const TerrainCapture& baseline,
                            const UnifiedLandscapeBuildScope& scope, PublishedTerrain& published, std::string& error);

// Legacy boundary conversion for parity tests only, never a runtime fallback:
// the path-lane outcome (already carrying its 0.25 m display totals) becomes
// the same neutral type. `operation`/`object` identify the designed path band.
// Per-domain rows attribute by owner alone (the legacy result carries no
// domain footprints); exact site/context stay zero because the legacy lane
// does not separate them from the displayed totals.
bool PublishTerrainDesign (const TerrainDesignOutcome& outcome, OperationId operation, ObjectId object,
                           std::span<const double> measurementRegion, PublishedTerrain& published, std::string& error);

} // namespace tapioca::geometry

#endif
