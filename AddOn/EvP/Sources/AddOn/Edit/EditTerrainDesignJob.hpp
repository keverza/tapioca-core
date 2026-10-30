#ifndef TAPIOCA_EDIT_TERRAIN_DESIGN_JOB_HPP
#define TAPIOCA_EDIT_TERRAIN_DESIGN_JOB_HPP

#include "Edit/EditDocument.hpp"
#include "Edit/EditTerrainDesign.hpp"
#include <future>
#include <memory>

namespace tapioca::geometry {
struct RegionTerrainResult;

// The widest a side slope's plan reach is widened to when it does not meet the ground, metres.
constexpr double kMaxSlopeReach = 100.0;

// The storage behind an EditTerrainInput, owned so a worker can outlive the
// host's capture buffers. Detached: the host stamps it from the document it was
// captured against; it is never document state.
struct TerrainCapture {
    std::shared_ptr<const TerrainCapture> native; // Full parent capture for a compacted native surface.
    std::vector<double> points;
    std::vector<uint32_t> triangles;
    ProvenanceRequest provenance; // Mesh vertices + faces (domains=5).
    uint64_t revision = 0;
    std::string sourceTerrainGuid;
    EditTerrainInput Input () const
    {
        return { { points, triangles }, provenance, revision, sourceTerrainGuid };
    }
};

// One immutable worker request. Everything it reads is owned or shared-const:
// the document snapshot, the capture and the options (region rebound at run).
struct TerrainDesignJob {
    uint64_t surfaceTable = 0; // Nonzero: extract a native solid's surface on the worker into this new table.
    uint64_t generation = 0;
    std::shared_ptr<const geomsrv::edit::Document> doc;
    std::shared_ptr<const TerrainCapture> capture;
    TerrainDesignOptions options; // `region` is ignored; see below.
    std::vector<double> region;   // Owned caller ring; empty derives the band's reach.
};

// What a worker produced, stamped with what it was computed against.
struct TerrainDesignOutcome {
    std::shared_ptr<const RegionTerrainResult>
        regions;                                   // Attributed area stage, retained through optional path composition.
    std::shared_ptr<const TerrainCapture> capture; // Retains full input/native ancestry behind the result.
    uint64_t generation = 0, revision = 0;
    std::array<uint8_t, 16> session {};
    std::string sourceTerrainGuid;
    bool built = false;
    TerrainDesignResult result;                   // When built.
    std::vector<TerrainDesignConflict> conflicts; // Station places and canonical fixed conflicts (if TIN exists).
    std::string error;                            // When refused.
    // What the strict design asked for and could not have, in the user's words: each condition
    // the worker relaxed to build anyway, then -- when it still refused -- the one left.
    std::vector<std::string> unmet;
};

// The condition a refusal code stands for, said plainly; the raw message when it has none.
std::string TerrainConditionText (const std::string& error);

// Worker side: pure, never throws, touches nothing shared. A refusal is an
// outcome too, with located band/fixed-control conflicts when available.
// It always tries to build (the user, 2026-10-24: "always try to create a mesh and in the
// panel just write which conditions are not satisfied"): a strict refusal it can work round is
// retried once per condition -- the region clipped to the captured terrain's footprint, the
// side slopes' reach widened (up to kMaxSlopeReach), finer stations -- and each one taken is
// listed in `unmet`. The result is then the relaxed design, never presented as the strict one.
TerrainDesignOutcome RunTerrainDesign (const TerrainDesignJob& job);
// Runs it on its own thread; the host polls the future from the main thread.
std::future<TerrainDesignOutcome> LaunchTerrainDesign (TerrainDesignJob job);

// Main-thread side: hands out jobs and publishes their outcomes into a derived
// cache (never document state, never history). An outcome publishes only when
// it is the latest generation requested AND the current document still has its
// session, revision and source terrain; otherwise it is discarded and the cache
// is left as it was. Not thread-safe by design: one owner, the main thread.
class TerrainDesignPublisher {
  public:
    // Snapshots `doc` and refuses a capture of another session, revision or
    // terrain (ProvenanceStale) before any worker runs. Each job supersedes the
    // previous one.
    bool Prepare (const geomsrv::edit::Document& doc, std::shared_ptr<const TerrainCapture> capture,
                  const TerrainDesignOptions& options, std::span<const double> region, TerrainDesignJob& job,
                  std::string& error);
    bool Publish (const geomsrv::edit::Document& current, TerrainDesignOutcome outcome, std::string& error);
    // The published outcome while it still describes `current`; null otherwise.
    const TerrainDesignOutcome* Current (const geomsrv::edit::Document& current) const;
    // Display only: the published outcome while it still belongs to `current`'s session and
    // terrain, whatever revision it was built for. Never a status or publication answer.
    const TerrainDesignOutcome* LastPublished (const geomsrv::edit::Document& current) const;
    uint64_t Latest () const
    {
        return latest;
    }

  private:
    uint64_t latest = 0;
    std::optional<TerrainDesignOutcome> published;
};

} // namespace tapioca::geometry
#endif
