#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudySampleSetup.hpp"

#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "SunStudy/SunStudyKernels.hpp"
#include "SunStudy/SunStudyLiftedPoint.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"

#include <cmath>
#include <cstring>
#include <tuple>

namespace geomsrv {

using sunstudysupport::AtlasRefusal;
using sunstudysupport::GroundGridRefusal;
using sunstudysupport::ReadDouble;
using sunstudysupport::ReadString;
using sunstudysupport::SurfaceSamplingRefusal;
using sunstudysupport::UnpackDoubles;

namespace {

// The layer replaces the triangle grid's samples with its available receptors
// and records what was left out. The sampling layout's per-mesh spans no longer
// partition the grid, so it is invalidated (the study is never a reuse source).
NativeCommandResult ApplyReceptorLayer (const SunStudySampleRequest& request, double normalOffset,
                                        evp::sunstudy::SampleGrid& samples, evp::sunstudy::StudyRecord& record,
                                        std::vector<evp::sunstudy::ReceptorCell>& unavailableCells)
{
    if (request.occluders == nullptr)
        return NativeCommandResult::Failure ("receptorHeight needs the study's occluders for the overhead test");
    const auto occluders = request.occluders;
    const evp::sunstudy::OverheadDistance overhead = [occluders] (const double host[3]) {
        return evp::sunstudy::SunStudyOverheadDistance (*occluders, host);
    };
    evp::sunstudy::ReceptorLayer layer;
    std::string error;
    if (!evp::sunstudy::BuildReceptorLayer (samples, normalOffset, request.receptor, &overhead, layer, error))
        return NativeCommandResult::Failure (GS::UniString ("the receptor layer was refused: ") +
                                             GS::UniString (error.c_str (), CC_UTF8));
    std::vector<evp::sunstudy::ReceptorCell> cells = evp::sunstudy::UnavailableReceptorCells (samples, layer);
    if (!evp::sunstudy::KeepAvailableReceptors (samples, layer))
        return NativeCommandResult::Failure ("the receptor layer does not match the sampled grid");
    record.receptorHeight = request.receptor.height;
    record.receptorUp = request.receptor.faceUp;
    record.receptorPlan = request.receptor.planArea;
    record.receptorMinHostUp = request.receptor.minHostUp;
    record.receptorCount = layer.Count ();
    record.receptorSteepHost = layer.steepHost;
    record.receptorBlocked = layer.blocked;
    record.receptorInvalidHost = layer.invalidHost;
    record.receptorUnavailableArea = layer.unavailableArea;
    record.samplingLayout.valid = false;
    record.samplingLayout.meshes.clear ();
    unavailableCells = std::move (cells);
    return GS::ObjectState ();
}

uint64_t HeightBits (double height)
{
    uint64_t bits = 0;
    std::memcpy (&bits, &height, sizeof bits);
    return bits;
}

} // namespace

SunStudyReceptorReport SunStudyReceptorReportOf (const evp::sunstudy::StudyRecord& record)
{
    SunStudyReceptorReport report;
    if (!record.HasReceptor ())
        return report;
    report.requested = true;
    report.height = record.receptorHeight;
    report.count = record.receptorCount;
    report.available = record.sampleGrid.Count ();
    report.steepHost = record.receptorSteepHost;
    report.blocked = record.receptorBlocked;
    report.invalidHost = record.receptorInvalidHost;
    for (const double area : record.sampleGrid.areas)
        report.availableArea += area;
    report.unavailableArea = record.receptorUnavailableArea;
    return report;
}

bool ReadSunStudyReceptor (const GS::ObjectState& params, bool& requested, evp::sunstudy::ReceptorOptions& options,
                           GS::UniString& error)
{
    options = evp::sunstudy::ReceptorOptions ();
    requested = params.Contains ("receptorHeight");
    if (!requested) {
        for (const char* key : { "receptorNormal", "receptorArea", "receptorMinHostUp" })
            if (params.Contains (key)) {
                error = GS::UniString (key) + " needs receptorHeight - it was refused, not ignored";
                return false;
            }
        return true;
    }
    options.height = ReadDouble (params, "receptorHeight", -1.0);
    if (!std::isfinite (options.height) || options.height < 0.0 || options.height > 100.0) {
        error = "receptorHeight must be metres above the host, from 0 to 100";
        return false;
    }
    const std::string normal = ReadString (params, "receptorNormal", "up");
    const std::string area = ReadString (params, "receptorArea", "plan");
    if ((normal != "up" && normal != "surface") || (area != "plan" && area != "surface")) {
        error = "receptorNormal must be 'up' or 'surface' and receptorArea 'plan' or 'surface'";
        return false;
    }
    options.faceUp = normal == "up";
    options.planArea = area == "plan";
    options.minHostUp = ReadDouble (params, "receptorMinHostUp", evp::sunstudy::kReceptorDefaultMinHostUp);
    if (!std::isfinite (options.minHostUp) || options.minHostUp <= 0.0 || options.minHostUp > 1.0) {
        error = "receptorMinHostUp must be a host-normal Z in (0, 1]";
        return false;
    }
    return true;
}

NativeCommandResult PrepareSunStudySamples (const GS::ObjectState& params, const Snapshot& snapshot,
                                            const SunStudySampleRequest& request, evp::sunstudy::StudyRecord& record,
                                            SunStudyPreparationTrace& trace, const std::function<bool ()>& isCancelled,
                                            SunStudySampleSetup& out)
{
    out = SunStudySampleSetup ();
    const std::string& sampleMode = request.sampleMode;
    const std::string& domainName = request.domainName;
    const bool sampleSurfaces = request.sampleSurfaces;
    const bool sampleExplicit = request.sampleExplicit;
    const bool patchDomain = request.patchDomain;
    const double spacing = request.spacing;
    const auto& clip = request.clip;
    const auto& limits = request.limits;
    const evp::sunstudy::StudyRecord* reuseSource = request.reuseSource;
    static const std::vector<uint8_t> kNoMask;
    static const std::vector<std::vector<uint8_t>> kNoFaceMasks;
    const std::vector<uint8_t>& sampleMask = request.sampleMask != nullptr ? *request.sampleMask : kNoMask;
    const std::vector<std::vector<uint8_t>>& faceMasks =
        request.faceMasks != nullptr ? *request.faceMasks : kNoFaceMasks;

    double snapshotMin[3] = { 0.0, 0.0, 0.0 };
    double snapshotMax[3] = { 0.0, 0.0, 0.0 };
    if (!SunStudySnapshotBounds (snapshot, snapshotMin, snapshotMax))
        return NativeCommandResult::Failure ("the snapshot has no vertices to bound");

    const double pad = ReadDouble (params, "pad", -1.0);
    const double zOffset = ReadDouble (params, "zOffset", 0.10);
    if (!patchDomain && domainName != "triangle") {
        return NativeCommandResult::Failure (GS::UniString ("'domain' must be 'triangle' or 'patch', not '") +
                                             GS::UniString (domainName.c_str (), CC_UTF8) + "'");
    }
    if (patchDomain && !sampleSurfaces) {
        // ⚠️ REFUSED RATHER THAN IGNORED. A patch is a coplanar run of the
        // MODEL's own triangles; a ground plane has none, and an explicit
        // point list is not a surface at all. Quietly falling back to the
        // triangle domain would hand back a study that says `domain=triangle`
        // only if the caller thought to read it -- and everything downstream
        // of here, the incremental cache included, keys on patches existing.
        return NativeCommandResult::Failure (
            GS::UniString ("domain='patch' needs samples='surfaces' - a patch is a coplanar run of the model's "
                           "own triangles, which samples='") +
            GS::UniString (sampleMode.c_str (), CC_UTF8) + "' does not produce");
    }

    if (request.receptorRequested && (!sampleSurfaces || patchDomain)) {
        // A receptor lifts the triangle lattice's own samples; the patch domain,
        // the ground grid and explicit points have no host lattice wired yet.
        return NativeCommandResult::Failure ("receptorHeight needs samples='surfaces' and domain='triangle'");
    }

    if (sampleExplicit) {
        // ⚠️ THE SEAM THAT MAKES THIS A CORE RATHER THAN A COMMAND. A
        // consumer that already has a sample set -- the browser page, a
        // graph node, a regression fixture -- must be able to measure THOSE
        // POINTS, because a cross-check between two engines is only
        // meaningful on an identical sample set. Given different points,
        // two correct engines still disagree and neither is at fault.
        GS::UniString packedPositions;
        GS::UniString packedNormals;
        if (params.Get ("positionsPacked", packedPositions) && !packedPositions.IsEmpty ()) {
            if (!UnpackDoubles (packedPositions, record.positions))
                return NativeCommandResult::Failure ("'positionsPacked' is not base64 of float64 triples");
            if (!params.Get ("normalsPacked", packedNormals) || !UnpackDoubles (packedNormals, record.normals))
                return NativeCommandResult::Failure ("'positionsPacked' needs a matching 'normalsPacked'");
        }
        else {
            GS::Array<double> inPositions;
            GS::Array<double> inNormals;
            params.Get ("positions", inPositions);
            params.Get ("normals", inNormals);
            record.positions.reserve (inPositions.GetSize ());
            for (USize i = 0; i < inPositions.GetSize (); ++i)
                record.positions.push_back (inPositions[i]);
            record.normals.reserve (inNormals.GetSize ());
            for (USize i = 0; i < inNormals.GetSize (); ++i)
                record.normals.push_back (inNormals[i]);
        }

        if (record.positions.empty () || record.positions.size () % 3 != 0) {
            return NativeCommandResult::Failure (
                "samples='explicit' needs 'positions' (or 'positionsPacked') as xyz triples");
        }
        if (record.normals.size () != record.positions.size ()) {
            return NativeCommandResult::Failure (
                "samples='explicit' needs one normal per position - without them every sample is treated as "
                "facing the sun, so the back of a wall counts the sun striking its front");
        }

        out.gridVersion = static_cast<uint64_t> (record.positions.size ()) * 73856093ull;
    }
    else if (sampleSurfaces) {
        // ⚠️ SURFACES ARE THE DEFAULT BECAUSE A GROUND PLANE CANNOT BE
        // INFERRED. The ground grid guesses a height from the snapshot AABB,
        // and the first live run showed exactly how that fails: on a project
        // whose only geometry was a 0.3 m slab, the plane landed INSIDE it
        // and every sample under the footprint reported nought hours. The
        // study passed every check and the picture looked like a shadow.
        // Sampling the model's own faces asks no such question, and it is
        // also what the browser-side study measures -- which is what lets
        // the two be diffed sample for sample rather than merely compared.
        evp::sunstudy::SurfaceSamplingOptions options;
        options.domain =
            patchDomain ? evp::sunstudy::SamplingDomain::SurfacePatch : evp::sunstudy::SamplingDomain::TriangleLegacy;
        options.maxSamples = limits.maxSamples;
        options.spacing = spacing;
        options.normalOffset = zOffset;
        options.jitter = ReadDouble (params, "jitter", 0.0);
        std::tie (options.clipX, options.clipY, options.clipRadius) = std::tie (clip.x, clip.y, clip.radius);
        auto sampling = evp::sunstudy::kernels::BuildSurfaceSampling (snapshot, sampleMask, options, reuseSource,
                                                                      isCancelled, faceMasks);
        trace.Mark ("sampling", (sampling.triangles.Count () + sampling.patches.Count ()) * 6 * sizeof (double));
        LogSunStudySurfaceSampling (snapshot.id, sampling);
        if (isCancelled ())
            return NativeCommandResult::Failure ("sun study preparation cancelled");
        if (!sampling.valid)
            return NativeCommandResult::Failure (SurfaceSamplingRefusal (sampling, limits, spacing));
        record.samplingLayout = std::move (sampling.layout);
        out.closedGroups = sampling.winding.closed;
        out.flippedGroups = sampling.winding.flipped;
        size_t faceCount = 0;
        for (const auto& mesh : snapshot.meshes)
            faceCount += mesh.triangles.size () / 3;

        if (patchDomain) {
            auto patches = std::move (sampling.patches);
            record.domain = evp::sunstudy::SamplingDomain::SurfacePatch;
            record.positions = patches.positions;
            record.normals = patches.normals;
            out.degenerateFaces = patches.degenerateFaces;
            out.excludedSurfaces = patches.excludedPatches;
            // Reported through the same field the triangle path uses for
            // faces too small to carry a lattice. Same meaning, same remedy.
            out.undersizedFaces = patches.centroidPatches;

            // Fit a copy of the compatible immutable source's allocations.
            if (reuseSource != nullptr && reuseSource->IsPatchDomain () && reuseSource->gridSpacing == spacing &&
                reuseSource->patchAtlas.Width () <= limits.maxAtlasDimension)
                record.patchAtlas = reuseSource->patchAtlas;
            evp::sunstudy::kernels::FitPatchAtlas (record.patchAtlas, patches, 1, limits.maxAtlasDimension);
            trace.Mark ("atlas", record.patchAtlas.TexelCount () * sizeof (float));
            if (record.patchAtlas.Width () == 0)
                return NativeCommandResult::Failure (AtlasRefusal ("patch", limits.maxAtlasDimension));
            out.atlasWidth = record.patchAtlas.Width ();
            out.atlasHeight = record.patchAtlas.Height ();
            out.atlasFaces = record.patchAtlas.AllocationCount ();

            out.gridVersion = static_cast<uint64_t> (patches.Count ()) * 73856093ull ^
                              static_cast<uint64_t> (patches.spans.size ()) * 19349663ull ^
                              static_cast<uint64_t> (faceCount * 3) * 83492791ull;
            out.patchCount = patches.spans.size ();
            record.patchGrid = std::move (patches);
        }
        else {
            auto samples = std::move (sampling.triangles);
            record.positions = samples.positions;
            record.normals = samples.normals;
            out.undersizedFaces = samples.undersizedFaces;
            out.degenerateFaces = samples.degenerateFaces;
            out.excludedSurfaces = samples.excludedFaces;
            std::vector<evp::sunstudy::ReceptorCell> unavailableCells;
            if (request.receptorRequested) {
                if (const NativeCommandResult lifted =
                        ApplyReceptorLayer (request, zOffset, samples, record, unavailableCells);
                    !lifted.ok)
                    return lifted;
                record.positions = samples.positions;
                record.normals = samples.normals;
            }
            record.atlas = evp::sunstudy::kernels::BuildStableTriangleAtlas (
                samples, snapshot, record.triangleAllocations,
                reuseSource != nullptr && !reuseSource->IsPatchDomain () && reuseSource->gridSpacing == spacing
                    ? &reuseSource->triangleAllocations
                    : nullptr,
                { 1, limits.maxAtlasDimension });
            if (!record.atlas.valid && samples.Count () != 0)
                return NativeCommandResult::Failure (AtlasRefusal ("triangle", limits.maxAtlasDimension));
            trace.Mark ("atlas", record.atlas.width * static_cast<size_t> (record.atlas.height) * sizeof (float));
            // Unavailable receptors keep their host face's tile; their texels
            // are drawn as "no answer" (kUnavailableTexel), never as shade.
            record.receptorUnavailableTexels = evp::sunstudy::ReceptorCellTexels (record.atlas, unavailableCells);
            out.atlasWidth = record.atlas.width;
            out.atlasHeight = record.atlas.height;
            out.atlasFaces = record.atlas.placedFaces;
            out.gridVersion = static_cast<uint64_t> (samples.Count ()) * 73856093ull ^
                              static_cast<uint64_t> (faceCount * 3) * 19349663ull;
            // Two layers can share every count; their heights must not share
            // an identity in the core's sync.
            if (request.receptorRequested)
                out.gridVersion ^= HeightBits (request.receptor.height) * 2654435761ull;
            record.sampleGrid = std::move (samples);
        } // end of the triangle domain
    }
    else {
        const evp::sunstudy::GroundGrid grid = evp::sunstudy::kernels::MakeGroundSampleGrid (
            evp::sunstudy::Vec3 { snapshotMin[0], snapshotMin[1], snapshotMin[2] },
            evp::sunstudy::Vec3 { snapshotMax[0], snapshotMax[1], snapshotMax[2] }, spacing, pad, zOffset,
            limits.maxSamples);
        if (!grid.valid)
            return NativeCommandResult::Failure (GroundGridRefusal (limits, spacing, snapshotMin, snapshotMax));

        record.positions = grid.positions;
        record.normals = grid.normals;
        out.columns = grid.columns;
        out.rows = grid.rows;
        out.groundZ = grid.groundZ;
        out.reportedPad = grid.pad;
        out.gridVersion =
            static_cast<uint64_t> (grid.columns) * 73856093ull ^ static_cast<uint64_t> (grid.rows) * 19349663ull;
    }
    return GS::ObjectState ();
}

} // namespace geomsrv
