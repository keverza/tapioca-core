#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/SunStudyCommands.hpp"
#include "NativeCommands/SunStudyCommandsSupport.hpp"
#include "NativeCommands/SunStudyPreparation.hpp"
#include "SunStudy/SunStudyOccluders.hpp"

#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudySampler.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"
#include "SunStudy/SunStudyPreset.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudyWinding.hpp"

#include <cstring>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {

namespace {

using evp::sunstudy::CpuTraversal;
using evp::sunstudy::StudyProgress;
using evp::sunstudy::StudyRecord;
using evp::sunstudy::SunSeries;
using evp::sunstudy::SunStep;
using evp::sunstudy::SunStudyStore;

// ⚠️ THE PARAMETER READING AND BULK PACKING LIVE IN
// NativeCommands/SunStudyCommandsSupport, NOT HERE. This domain and the
// display domain are separate files because one command file exports exactly
// one provider -- but they speak the same wire format, and two private copies
// of a base64 packer is how two callers of "the same" format begin to
// disagree about it. `using` rather than qualification at every call site,
// because these read as language here.
using sunstudysupport::LimitRefusal;
using sunstudysupport::MachineAnalysisLimits;
using sunstudysupport::PackBits;
using sunstudysupport::PackDoubles;
using sunstudysupport::ReadDouble;
using sunstudysupport::ReadInt;
using sunstudysupport::ReadString;
using sunstudysupport::ReadStringList;
using sunstudysupport::ReadStudyId;
using sunstudysupport::Text;
using sunstudysupport::UnpackDoubles;
using sunstudysupport::Utf8;

// ⚠️ THE PROGRESS FIELDS ARE WRITTEN OUT IN EVERY COMMAND RATHER THAN THROUGH A
// HELPER, AND THAT IS DELIBERATE. tools/schema_check.py reads the text of each
// ExecuteNative to prove that every field the response schema REQUIRES is
// actually added; a field contributed by a file-local helper is invisible to it,
// so the gate would pass a command whose every call then fails validation at
// runtime. That gate has already cost two live runs (see its header), and
// hiding six fields from it to save four repetitions is a bad trade.
//
// ⚠️ BOTH FLAGS, ALWAYS, wherever they appear below. `converged` alone cannot
// distinguish a finished study from one that had nothing to analyse -- both
// report zero hours everywhere, and only `empty` separates them.

// ---------------------------------------------------------------------------
// Tapioca.StartSunStudy
//
// MAIN THREAD, because the sun comes from Archicad and nothing else may compute
// it. It gathers the whole day's vectors ONCE, so no later call needs the host.
// ---------------------------------------------------------------------------
class StartSunStudyCommand : public MainThreadCommand {
  public:
    StartSunStudyCommand () = default;
    StartSunStudyCommand (std::shared_ptr<const CapturedSunStudyInputs> captured, const std::atomic<bool>* cancelled,
                          std::shared_ptr<const StudyRecord> reuseSource = nullptr)
        : captured_ (std::move (captured)), cancelled_ (cancelled), reuseSource_ (std::move (reuseSource))
    {
    }
    GS::String GetName () const override
    {
        return "StartSunStudy";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        auto captured = captured_;
        if (captured == nullptr) {
            const NativeCommandResult capture = CaptureSunStudyInputs (params, captured);
            if (!capture.ok)
                return capture;
        }
        const std::shared_ptr<const Snapshot>& snapshot = captured->snapshot;
        SunStudyPreparationTrace trace (snapshot->id);
        if (IsCancelled ())
            return NativeCommandResult::Failure ("sun study preparation cancelled");

        // ---- which elements are MEASURED, and which only cast shadow ---------
        //
        // Every element is one material to the analysis; the distinction that
        // matters is its ROLE. See SunStudy/SunStudyRoles.hpp for the table --
        // naming nothing reproduces the study as it was before roles existed.
        const auto& binding = captured->selectionBinding;
        const auto analysisPicked =
            binding.analysisSet.empty () ? ReadStringList (params, "analysisElements") : captured->analysisElements;
        const auto contextPicked =
            binding.contextSet.empty () ? ReadStringList (params, "contextElements") : captured->contextElements;
        const auto ignoredPicked =
            binding.ignoredSet.empty () ? ReadStringList (params, "ignoredElements") : captured->ignoredElements;
        const auto requestedPreset = ReadString (params, "preset", "");
        if (!requestedPreset.empty () && requestedPreset != "early" && requestedPreset != "late")
            return NativeCommandResult::Failure ("preset must be early or late");
        if (!requestedPreset.empty () && ReadString (params, "samples", "surfaces") != "surfaces")
            return NativeCommandResult::Failure ("analysis presets require samples='surfaces'");
        const bool analysisRestricted =
            !binding.analysisSet.empty () || (!requestedPreset.empty () && params.Contains ("analysisElements"));
        if (analysisRestricted && analysisPicked.empty ())
            return NativeCommandResult::Failure (
                "Analysis picks are empty - pick receiver elements or use model scope; the study was not widened");
        const double glassThreshold = ReadDouble (params, "glassThreshold", 0.4);
        if (!std::isfinite (glassThreshold) || glassThreshold < 0.0 || glassThreshold > 1.0)
            return NativeCommandResult::Failure ("glassThreshold must be a transparency fraction from 0 to 1");
        const auto preset = requestedPreset;
        const std::string presetReason = "manual selection";
        // In a design preset, all unpicked geometry is CONTEXT, not implicitly
        // ignored just because the user also named some Context elements.
        auto roles = evp::sunstudy::ResolveElementRoles (
            *snapshot, analysisPicked,
            !preset.empty () && !analysisPicked.empty () ? std::vector<std::string> {} : contextPicked, ignoredPicked);
        if (roles.analysisNamedButAbsent) {
            // ⚠️ REFUSED, NOT WIDENED. An empty intersection is not an empty
            // list: "measure these" with none of them present must not become
            // "measure everything".
            return NativeCommandResult::Failure (
                GS::UniString ("none of the ") + GS::UniString::Printf ("%u", (unsigned) analysisPicked.size ()) +
                " analysis element(s) is left to measure - they are absent from the snapshot or all ignored");
        }
        const auto receivers = evp::sunstudy::BuildSunStudyReceivers (*snapshot, roles, preset == "late",
                                                                      captured->materialTransparency, glassThreshold);
        if (preset == "late" && receivers.analysisFaces == 0)
            return NativeCommandResult::Failure (
                "no transparency-qualified glass faces in the analysis scope - adjust glassThreshold or picks, or "
                "manually choose early; no whole-model fallback was run");
        roles = receivers.roles;
        // Per element (= per sampler group), whether its faces are measured.
        const std::vector<uint8_t> sampleMask = roles.SampleMask ();

        const auto occluderParts =
            PrepareSunStudyOccluders (snapshot, roles, reuseSource_.get (), [this] { return IsCancelled (); });
        if (occluderParts == nullptr)
            return NativeCommandResult::Failure ("sun study occluder preparation cancelled or invalid");
        const auto& occluders = occluderParts->analysis;
        trace.Mark ("roles-occluders");

        const API_PlaceInfo& place = captured->place;

        const GS::Int32 year = ReadInt (params, "year", place.year);
        const GS::Int32 month = ReadInt (params, "month", place.month);
        const GS::Int32 day = ReadInt (params, "day", place.day);
        const GS::Int32 timestep = std::max<GS::Int32> (1, ReadInt (params, "timestep", 60));
        const GS::Int32 hourFrom = ReadInt (params, "hourFrom", 0);
        const GS::Int32 hourTo = ReadInt (params, "hourTo", 24);
        const double minAltitude = ReadDouble (params, "minAltitudeDeg", 0.0);

        auto record = std::make_unique<StudyRecord> ();
        record->series = captured->series;
        record->timestepMinutes = timestep;
        record->year = year;
        record->month = month;
        record->day = day;
        record->hourFrom = hourFrom;
        record->hourTo = hourTo;
        record->minAltitudeDegrees = minAltitude;
        record->sourceStepCount = record->series.SourceStepCount ();

        // ---- the samples -----------------------------------------------------
        double snapshotMin[3] = { 0.0, 0.0, 0.0 };
        double snapshotMax[3] = { 0.0, 0.0, 0.0 };
        if (!SunStudySnapshotBounds (*snapshot, snapshotMin, snapshotMax))
            return NativeCommandResult::Failure ("the snapshot has no vertices to bound");

        const double spacing = ReadDouble (params, "grid", 2.0);
        const double pad = ReadDouble (params, "pad", -1.0);
        const double zOffset = ReadDouble (params, "zOffset", 0.10);
        const std::string sampleMode = ReadString (params, "samples", "surfaces");
        const bool sampleSurfaces = (sampleMode == "surfaces");
        const bool sampleExplicit = (sampleMode == "explicit");

        // ---- which DOMAIN the surfaces are diced into --------------------
        //
        // ⚠️ A DOMAIN IS NOT A SAMPLE MODE, which is why it is a separate
        // parameter rather than a fourth `samples` value. `samples` says WHAT is
        // measured -- the model's faces, a ground plane, or points the caller
        // supplies. `domain` says how those faces are DIVIDED: per source
        // triangle, or per coplanar surface. Folding them into one enum would
        // have made "ground, patch" spellable, and it means nothing.
        const std::string domainName = ReadString (params, "domain", "triangle");
        const bool patchDomain = (domainName == "patch");
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

        size_t columns = 0;
        size_t rows = 0;
        double groundZ = 0.0;
        double reportedPad = 0.0;
        uint64_t gridVersion = 0;
        size_t undersizedFaces = 0;
        size_t degenerateFaces = 0;
        size_t closedGroups = 0;
        size_t flippedGroups = 0;
        uint32_t atlasWidth = 0;
        uint32_t atlasHeight = 0;
        size_t atlasFaces = 0;
        size_t patchCount = 0;
        size_t excludedSurfaces = 0; // context faces (triangle) or surfaces (patch)

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
                if (!UnpackDoubles (packedPositions, record->positions))
                    return NativeCommandResult::Failure ("'positionsPacked' is not base64 of float64 triples");
                if (!params.Get ("normalsPacked", packedNormals) || !UnpackDoubles (packedNormals, record->normals))
                    return NativeCommandResult::Failure ("'positionsPacked' needs a matching 'normalsPacked'");
            }
            else {
                GS::Array<double> inPositions;
                GS::Array<double> inNormals;
                params.Get ("positions", inPositions);
                params.Get ("normals", inNormals);
                record->positions.reserve (inPositions.GetSize ());
                for (USize i = 0; i < inPositions.GetSize (); ++i)
                    record->positions.push_back (inPositions[i]);
                record->normals.reserve (inNormals.GetSize ());
                for (USize i = 0; i < inNormals.GetSize (); ++i)
                    record->normals.push_back (inNormals[i]);
            }

            if (record->positions.empty () || record->positions.size () % 3 != 0) {
                return NativeCommandResult::Failure (
                    "samples='explicit' needs 'positions' (or 'positionsPacked') as xyz triples");
            }
            if (record->normals.size () != record->positions.size ()) {
                return NativeCommandResult::Failure (
                    "samples='explicit' needs one normal per position - without them every sample is treated as "
                    "facing the sun, so the back of a wall counts the sun striking its front");
            }

            gridVersion = static_cast<uint64_t> (record->positions.size ()) * 73856093ull;
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
            const auto limits = MachineAnalysisLimits (record->series.StepCount (), patchDomain);
            evp::sunstudy::SurfaceSamplingOptions options;
            options.domain = patchDomain ? evp::sunstudy::SamplingDomain::SurfacePatch
                                         : evp::sunstudy::SamplingDomain::TriangleLegacy;
            options.maxSamples = limits.maxSamples;
            options.spacing = spacing;
            options.normalOffset = zOffset;
            options.jitter = ReadDouble (params, "jitter", 0.0);
            auto sampling = evp::sunstudy::BuildSurfaceSampling (
                *snapshot, sampleMask, options, reuseSource_.get (), [this] { return IsCancelled (); },
                receivers.faces);
            trace.Mark ("sampling", (sampling.triangles.Count () + sampling.patches.Count ()) * 6 * sizeof (double));
            LogSunStudySurfaceSampling (snapshot->id, sampling);
            if (IsCancelled ())
                return NativeCommandResult::Failure ("sun study preparation cancelled");
            if (!sampling.valid)
                return NativeCommandResult::Failure (LimitRefusal (limits, spacing));
            record->samplingLayout = std::move (sampling.layout);
            closedGroups = sampling.winding.closed;
            flippedGroups = sampling.winding.flipped;
            size_t faceCount = 0;
            for (const auto& mesh : snapshot->meshes)
                faceCount += mesh.triangles.size () / 3;

            if (patchDomain) {
                auto patches = std::move (sampling.patches);
                record->domain = evp::sunstudy::SamplingDomain::SurfacePatch;
                record->positions = patches.positions;
                record->normals = patches.normals;
                degenerateFaces = patches.degenerateFaces;
                excludedSurfaces = patches.excludedPatches;
                // Reported through the same field the triangle path uses for
                // faces too small to carry a lattice. Same meaning, same remedy.
                undersizedFaces = patches.centroidPatches;

                // ⚠️ FITTED ONCE, HERE, AND KEPT FOR THE STUDY'S LIFETIME. Every
                // later update re-fits THIS atlas, which is what keeps an
                // untouched surface's rectangle where it was. A fresh atlas per
                // read would repack and invalidate every texture coordinate
                // already handed out -- and the result draws perfectly, in
                // somebody else's colours.
                record->patchAtlas.Fit (patches, 1, limits.maxAtlasDimension);
                trace.Mark ("atlas", record->patchAtlas.TexelCount () * sizeof (float));
                if (record->patchAtlas.Width () == 0) {
                    return NativeCommandResult::Failure (
                        "the patch atlas could not be packed within the maximum texture dimension - ask for a "
                        "coarser grid");
                }
                atlasWidth = record->patchAtlas.Width ();
                atlasHeight = record->patchAtlas.Height ();
                atlasFaces = record->patchAtlas.AllocationCount ();

                gridVersion = static_cast<uint64_t> (patches.Count ()) * 73856093ull ^
                              static_cast<uint64_t> (patches.spans.size ()) * 19349663ull ^
                              static_cast<uint64_t> (faceCount * 3) * 83492791ull;
                patchCount = patches.spans.size ();
                record->patchGrid = std::move (patches);
            }
            else {

                auto samples = std::move (sampling.triangles);
                record->positions = samples.positions;
                record->normals = samples.normals;
                undersizedFaces = samples.undersizedFaces;
                degenerateFaces = samples.degenerateFaces;
                excludedSurfaces = samples.excludedFaces;

                // ⚠️ BUILT ONCE, HERE, BESIDE THE SAMPLES IT DESCRIBES. The packing
                // is a pure function of the sample grid, so rebuilding it per read
                // would produce a different arrangement and silently invalidate
                // every texture coordinate already handed to a consumer.
                record->atlas = evp::sunstudy::BuildSunStudyAtlas (samples);
                trace.Mark ("atlas", record->atlas.width * static_cast<size_t> (record->atlas.height) * sizeof (float));
                atlasWidth = record->atlas.width;
                atlasHeight = record->atlas.height;
                atlasFaces = record->atlas.placedFaces;
                gridVersion = static_cast<uint64_t> (samples.Count ()) * 73856093ull ^
                              static_cast<uint64_t> (faceCount * 3) * 19349663ull;
                record->sampleGrid = std::move (samples);
            } // end of the triangle domain
        }
        else {
            const evp::sunstudy::GroundGrid grid = evp::sunstudy::MakeGroundSampleGrid (
                evp::sunstudy::Vec3 { snapshotMin[0], snapshotMin[1], snapshotMin[2] },
                evp::sunstudy::Vec3 { snapshotMax[0], snapshotMax[1], snapshotMax[2] }, spacing, pad, zOffset);
            if (!grid.valid) {
                return NativeCommandResult::Failure (
                    "the ground sample grid was refused - check that grid spacing is positive and not so fine that "
                    "the site exceeds the sample ceiling");
            }

            record->positions = grid.positions;
            record->normals = grid.normals;
            columns = grid.columns;
            rows = grid.rows;
            groundZ = grid.groundZ;
            reportedPad = grid.pad;
            gridVersion =
                static_cast<uint64_t> (grid.columns) * 73856093ull ^ static_cast<uint64_t> (grid.rows) * 19349663ull;
        }

        if (IsCancelled ())
            return NativeCommandResult::Failure ("sun study preparation cancelled");
        record->gridSpacing = spacing;
        record->groundPad = reportedPad;
        // The OCCLUDERS: the whole snapshot, or the analysis + context subset.
        record->occluders = occluderParts;
        record->traversal =
            std::make_shared<evp::sunstudy::SunStudyPartitionTraversal> (occluders, occluderParts->context);
        // Kept so a follower rerun measures the same elements the same way.
        record->analysisElements = analysisPicked;
        record->contextElements = contextPicked;
        record->ignoredElements = ignoredPicked;
        record->selectionBinding = binding;
        record->preset = preset;
        record->glassThreshold = glassThreshold;
        record->analysisRestricted = analysisRestricted;
        record->snapshotId = snapshot->id;
        for (const evp::sunstudy::ElementRole role : roles.roles)
            record->elementRoles.push_back (static_cast<uint8_t> (role));

        evp::sunstudy::StudyInputs inputs;
        inputs.geometryVersion = snapshot->id;
        inputs.sunVersion = record->series.Version ();
        inputs.gridVersion = gridVersion;
        record->session.Sync (inputs, record->series, record->Samples ());
        FinishSunStudyPreparation (*record, snapshot, reuseSource_.get (), cancelled_, occluders);
        trace.Mark ("reuse-seed", record->session.Accumulator ().Bits ().size () * sizeof (uint64_t));

        const StudyProgress progress = record->session.Progress ();
        const double daylightHours = record->series.DaylightHours ();
        const size_t sourceSteps = record->sourceStepCount;

        if (IsCancelled ())
            return NativeCommandResult::Failure ("sun study preparation cancelled");
        const std::string id = SunStudyStore::Get ().Insert (std::move (record));
        if (id.empty ())
            return NativeCommandResult::Failure ("the sun study could not be stored");

        GS::ObjectState os;
        os.Add ("studyId", Text (id));
        os.Add ("resolvedSteps", (GS::Int32) progress.resolvedSteps);
        os.Add ("totalSteps", (GS::Int32) progress.totalSteps);
        os.Add ("sampleCount", (GS::Int32) progress.sampleCount);
        os.Add ("generation", (GS::Int32) progress.generation);
        os.Add ("converged", progress.converged);
        os.Add ("empty", progress.empty);
        os.Add ("daylightHours", daylightHours);
        os.Add ("sourceStepCount", (GS::Int32) sourceSteps);
        os.Add ("gridColumns", (GS::Int32) columns);
        os.Add ("gridRows", (GS::Int32) rows);
        os.Add ("groundZ", groundZ);
        os.Add ("groundPad", reportedPad);
        os.Add ("sampleMode", Text (sampleMode));
        // ⚠️ ECHOED BACK RATHER THAN ASSUMED. A caller that asked for `patch`
        // and silently got `triangle` would be reading a study whose every number
        // is reasonable and whose atlas cannot be updated incrementally.
        os.Add ("domain", Text (patchDomain ? std::string ("patch") : std::string ("triangle")));
        os.Add ("patchCount", (GS::Int32) patchCount);
        // ⚠️ THE ROLES AS RESOLVED, NOT AS ASKED. A picked element the snapshot
        // does not hold is counted, never dropped in silence.
        os.Add ("analysisElementCount", (GS::Int32) roles.analysis);
        os.Add ("contextElementCount", (GS::Int32) roles.context);
        os.Add ("ignoredElementCount", (GS::Int32) roles.ignored);
        os.Add ("unmatchedAnalysis", (GS::Int32) roles.unmatchedAnalysis);
        os.Add ("unmatchedContext", (GS::Int32) roles.unmatchedContext);
        os.Add ("unmatchedIgnored", (GS::Int32) roles.unmatchedIgnored);
        os.Add ("excludedSurfaces", (GS::Int32) excludedSurfaces);
        os.Add ("preset", Text (preset));
        os.Add ("presetReason", Text (presetReason));
        os.Add ("analysisFaceCount", (GS::Int32) receivers.analysisFaces);
        os.Add ("contextFaceCount", (GS::Int32) receivers.contextFaces);
        os.Add ("unknownMaterialFaces", (GS::Int32) receivers.unknownMaterialFaces);
        os.Add ("undersizedFaces", (GS::Int32) undersizedFaces);
        os.Add ("degenerateFaces", (GS::Int32) degenerateFaces);
        os.Add ("closedGroups", (GS::Int32) closedGroups);
        os.Add ("flippedGroups", (GS::Int32) flippedGroups);
        os.Add ("atlasWidth", (GS::Int32) atlasWidth);
        os.Add ("atlasHeight", (GS::Int32) atlasHeight);
        os.Add ("atlasFaces", (GS::Int32) atlasFaces);
        os.Add ("latitude", place.latitude);
        os.Add ("longitude", place.longitude);
        os.Add ("northDeg", place.north * 180.0 / 3.14159265358979323846);
        os.Add ("year", year);
        os.Add ("month", month);
        os.Add ("day", day);
        os.Add ("timestep", timestep);
        return os;
    }

  private:
    bool IsCancelled () const
    {
        return cancelled_ != nullptr && cancelled_->load ();
    }
    std::shared_ptr<const CapturedSunStudyInputs> captured_;
    const std::atomic<bool>* cancelled_ = nullptr;
    std::shared_ptr<const StudyRecord> reuseSource_;
};

// ---------------------------------------------------------------------------
// Tapioca.AdvanceSunStudy
//
// ⚠️ GATE-FREE, AND THAT IS THE POINT OF THE WHOLE SPLIT. This is the expensive
// call; running it on the host's main thread is precisely what would stutter the
// application. It touches no ACAPI -- only the immutable snapshot BVH the study
// already holds.
// ---------------------------------------------------------------------------
class AdvanceSunStudyCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "AdvanceSunStudy";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string id = ReadStudyId (params);
        if (id.empty ())
            return NativeCommandResult::Failure ("no sun study is live - call Tapioca.StartSunStudy first");

        // ⚠️ THE DEFAULT SLICE IS SMALL BECAUSE THE CALLER'S BUDGET IS UNKNOWN.
        // A caller that wants the whole study in one call asks for it; one that
        // wants to stay responsive does not have to know to ask for less.
        const GS::Int32 maxSteps = std::max<GS::Int32> (1, ReadInt (params, "maxSteps", 4));
        const GS::Int32 maxParallel = std::max<GS::Int32> (0, ReadInt (params, "maxParallel", 0));
        const double tmin = ReadDouble (params, "tmin", 0.001);
        const double tmax = ReadDouble (params, "tmax", 0.0);

        size_t advanced = 0;
        std::string error;
        if (!SunStudyStore::Get ().Advance (id, (size_t) maxSteps, (size_t) maxParallel, tmin, tmax, advanced, error))
            return NativeCommandResult::Failure (Text (error));

        StudyProgress progress;
        if (!SunStudyStore::Get ().Progress (id, progress, error))
            return NativeCommandResult::Failure (Text (error));

        StudyRecord metadata;
        SunStudyStore::Get ().Describe (id, metadata, error);

        GS::ObjectState os;
        os.Add ("studyId", Text (id));
        os.Add ("advanced", (GS::Int32) advanced);
        os.Add ("resolvedSteps", (GS::Int32) progress.resolvedSteps);
        os.Add ("totalSteps", (GS::Int32) progress.totalSteps);
        os.Add ("sampleCount", (GS::Int32) progress.sampleCount);
        os.Add ("generation", (GS::Int32) progress.generation);
        os.Add ("converged", progress.converged);
        os.Add ("empty", progress.empty);
        os.Add ("analysisMilliseconds", metadata.analysisMilliseconds);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SunStudyState — progress and parameters, cheap enough to poll.
// ---------------------------------------------------------------------------
class SunStudyStateCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "SunStudyState";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        const std::vector<std::string> ids = SunStudyStore::Get ().Ids ();
        GS::Array<GS::UniString> idArray;
        for (const std::string& each : ids)
            idArray.Push (Text (each));
        os.Add ("studyIds", idArray);
        os.Add ("studyCount", (GS::Int32) ids.size ());

        const std::string id = ReadStudyId (params);
        if (id.empty ()) {
            os.Add ("studyId", GS::UniString ());
            os.Add ("live", false);
            return os;
        }

        StudyProgress progress;
        std::string error;
        if (!SunStudyStore::Get ().Progress (id, progress, error))
            return NativeCommandResult::Failure (Text (error));

        StudyRecord metadata;
        SunStudyStore::Get ().Describe (id, metadata, error);

        os.Add ("studyId", Text (id));
        os.Add ("live", true);
        os.Add ("resolvedSteps", (GS::Int32) progress.resolvedSteps);
        os.Add ("totalSteps", (GS::Int32) progress.totalSteps);
        os.Add ("sampleCount", (GS::Int32) progress.sampleCount);
        os.Add ("generation", (GS::Int32) progress.generation);
        os.Add ("converged", progress.converged);
        os.Add ("empty", progress.empty);
        os.Add ("timestep", metadata.timestepMinutes);
        os.Add ("year", metadata.year);
        os.Add ("month", metadata.month);
        os.Add ("day", metadata.day);
        os.Add ("hourFrom", metadata.hourFrom);
        os.Add ("hourTo", metadata.hourTo);
        os.Add ("minAltitudeDeg", metadata.minAltitudeDegrees);
        os.Add ("grid", metadata.gridSpacing);
        os.Add ("groundPad", metadata.groundPad);
        os.Add ("sourceStepCount", (GS::Int32) metadata.sourceStepCount);
        os.Add ("analysisMilliseconds", metadata.analysisMilliseconds);
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.GetSunStudyResults
//
// Flat parallel arrays, the shape bulk numerics have used since E2: nested
// records are for element reads, flat arrays for volumes like this.
// ---------------------------------------------------------------------------
class GetSunStudyResultsCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetSunStudyResults";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const std::string id = ReadStudyId (params);
        if (id.empty ())
            return NativeCommandResult::Failure ("no sun study is live - call Tapioca.StartSunStudy first");

        bool wantSteps = false;
        params.Get ("includeSteps", wantSteps);
        bool packed = false;
        params.Get ("packed", packed);

        std::vector<double> hours;
        std::vector<double> positions;
        std::vector<double> normals;
        std::vector<uint8_t> stepBits;
        std::string error;
        if (!SunStudyStore::Get ().Results (id, hours, positions, normals, wantSteps ? &stepBits : nullptr, error))
            return NativeCommandResult::Failure (Text (error));

        StudyProgress progress;
        SunStudyStore::Get ().Progress (id, progress, error);

        GS::Array<double> hoursArray;
        for (const double value : hours)
            hoursArray.Push (value);

        GS::ObjectState os;
        os.Add ("studyId", Text (id));
        os.Add ("hours", hoursArray);
        os.Add ("count", (GS::Int32) hours.size ());
        os.Add ("resolvedSteps", (GS::Int32) progress.resolvedSteps);
        os.Add ("totalSteps", (GS::Int32) progress.totalSteps);
        os.Add ("sampleCount", (GS::Int32) progress.sampleCount);
        os.Add ("generation", (GS::Int32) progress.generation);
        os.Add ("converged", progress.converged);
        os.Add ("empty", progress.empty);

        // ⚠️ POSITIONS ARE OPTIONAL AND OFF BY DEFAULT. They are three doubles
        // per sample against one for the hours, so shipping them on every poll
        // triples the wire cost of a value that never changes during a study.
        bool wantPositions = false;
        if (params.Get ("includePositions", wantPositions) && wantPositions) {
            if (packed) {
                os.Add ("positionsPacked", PackDoubles (positions));
                os.Add ("normalsPacked", PackDoubles (normals));
            }
            else {
                GS::Array<double> positionArray;
                for (const double value : positions)
                    positionArray.Push (value);
                os.Add ("positions", positionArray);

                // ⚠️ NORMALS TRAVEL WITH POSITIONS, NEVER SEPARATELY. A consumer
                // that has the points but not their orientation cannot reproduce
                // the back-face cull, so it counts the sun striking the far side of
                // every wall -- and then disagrees with this engine on exactly the
                // samples the cull would have settled, which reads as a tracer bug.
                GS::Array<double> normalArray;
                for (const double value : normals)
                    normalArray.Push (value);
                os.Add ("normals", normalArray);
            }
        }

        bool wantAtlas = false;
        if (params.Get ("includeAtlas", wantAtlas) && wantAtlas) {
            uint32_t atlasWidth = 0;
            uint32_t atlasHeight = 0;
            std::vector<float> image;
            std::string atlasError;
            // ⚠️ WHICHEVER ATLAS THIS STUDY ACTUALLY HAS. The two are packed by
            // different allocators, so a caller cannot be handed one while
            // believing it has the other -- the picture would draw perfectly,
            // with every surface reading a stranger's hours. `atlasDomain` says
            // which arrived; a reader that ignores it is reading coordinates it
            // cannot interpret.
            const bool patchAtlas =
                SunStudyStore::Get ().PatchAtlasImage (id, atlasWidth, atlasHeight, image, atlasError);
            if (patchAtlas || SunStudyStore::Get ().AtlasImage (id, atlasWidth, atlasHeight, image, atlasError)) {
                os.Add ("atlasDomain", Text (patchAtlas ? std::string ("patch") : std::string ("triangle")));
                os.Add ("atlasWidth", (GS::Int32) atlasWidth);
                os.Add ("atlasHeight", (GS::Int32) atlasHeight);
                // float32, row-major, negative in every texel no sample reached.
                std::vector<unsigned char> bytes (image.size () * sizeof (float));
                if (!image.empty ())
                    std::memcpy (bytes.data (), image.data (), bytes.size ());
                os.Add ("atlasPacked", Base64Encode (bytes));
            }
            else {
                // ⚠️ REPORTED, NOT SILENT. A ground-plane study legitimately has
                // no atlas; a caller that got an empty field with no reason
                // would read it as "no sun anywhere".
                os.Add ("atlasReason", Text (atlasError));
            }
        }

        if (wantSteps) {
            os.Add ("stepStride", (GS::Int32) progress.totalSteps);
            if (packed) {
                os.Add ("stepBitsPacked", PackBits (stepBits));
            }
            else {
                GS::Array<GS::Int32> stepArray;
                for (const uint8_t value : stepBits)
                    stepArray.Push ((GS::Int32) value);
                os.Add ("stepBits", stepArray);
            }
        }
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.CancelSunStudy — forget a study, or all of them.
// ---------------------------------------------------------------------------
class CancelSunStudyCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "CancelSunStudy";
    }
    bool NeedsMainThread () const override
    {
        return false;
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        bool all = false;
        if (params.Get ("all", all) && all) {
            const size_t erased = SunStudyStore::Get ().Count ();
            SunStudyStore::Get ().Clear ();
            os.Add ("erased", (GS::Int32) erased);
            return os;
        }

        const std::string id = ReadStudyId (params);
        os.Add ("studyId", Text (id));
        os.Add ("erased", (GS::Int32) (id.empty () ? 0 : (SunStudyStore::Get ().Erase (id) ? 1 : 0)));
        return os;
    }
};

// ---------------------------------------------------------------------------

const NativeCommandRegistration kSunStudyRegistrations[] = {
    { "StartSunStudy", &MakeRegisteredNativeCommand<StartSunStudyCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "year":{"type":"integer"},
                "month":{"type":"integer"},
                "day":{"type":"integer"},
                "timestep":{"type":"integer"},
                "hourFrom":{"type":"integer"},
                "hourTo":{"type":"integer"},
                "minAltitudeDeg":{"type":"number"},
                "grid":{"type":"number"},
                "pad":{"type":"number"},
                "zOffset":{"type":"number"},
                "samples":{"type":"string","enum":["surfaces","ground","explicit"]},
                "domain":{"type":"string","enum":["triangle","patch"]},
                "preset":{"type":"string","enum":["early","late"]},
                "glassThreshold":{"type":"number","minimum":0,"maximum":1},
                "analysisSelectionSet":{"type":"string","minLength":1},
                "analysisElements":{"type":"array","items":{"type":"string","minLength":1}},
                "contextElements":{"type":"array","items":{"type":"string","minLength":1}},
                "ignoredElements":{"type":"array","items":{"type":"string","minLength":1}},
                "contextSelectionSet":{"type":"string","minLength":1},
                "ignoredSelectionSet":{"type":"string","minLength":1},
                "positions":{"type":"array","items":{"type":"number"}},
                "normals":{"type":"array","items":{"type":"number"}},
                "positionsPacked":{"type":"string"},
                "normalsPacked":{"type":"string"},
                "jitter":{"type":"number"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "resolvedSteps":{"type":"integer"},
                "totalSteps":{"type":"integer"},
                "sampleCount":{"type":"integer"},
                "generation":{"type":"integer"},
                "converged":{"type":"boolean"},
                "empty":{"type":"boolean"},
                "daylightHours":{"type":"number"},
                "sourceStepCount":{"type":"integer"},
                "gridColumns":{"type":"integer"},
                "gridRows":{"type":"integer"},
                "groundZ":{"type":"number"},
                "groundPad":{"type":"number"},
                "sampleMode":{"type":"string"},
                "undersizedFaces":{"type":"integer"},
                "degenerateFaces":{"type":"integer"},
                "closedGroups":{"type":"integer"},
                "flippedGroups":{"type":"integer"},
                "atlasWidth":{"type":"integer"},
                "atlasHeight":{"type":"integer"},
                "atlasFaces":{"type":"integer"},
                "domain":{"type":"string"},
                "patchCount":{"type":"integer"},
                "analysisElementCount":{"type":"integer"},
                "contextElementCount":{"type":"integer"},
                "ignoredElementCount":{"type":"integer"},
                "unmatchedAnalysis":{"type":"integer"},
                "unmatchedContext":{"type":"integer"},
                "unmatchedIgnored":{"type":"integer"},
                "excludedSurfaces":{"type":"integer"},
                "preset":{"type":"string"},
                "presetReason":{"type":"string"},
                "analysisFaceCount":{"type":"integer"},
                "contextFaceCount":{"type":"integer"},
                "unknownMaterialFaces":{"type":"integer"},
                "latitude":{"type":"number"},
                "longitude":{"type":"number"},
                "northDeg":{"type":"number"},
                "year":{"type":"integer"},
                "month":{"type":"integer"},
                "day":{"type":"integer"},
                "timestep":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["studyId","resolvedSteps","totalSteps","sampleCount","converged","empty"]
        })json" },
    { "AdvanceSunStudy", &MakeRegisteredNativeCommand<AdvanceSunStudyCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "maxSteps":{"type":"integer"},
                "maxParallel":{"type":"integer"},
                "tmin":{"type":"number"},
                "tmax":{"type":"number"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "advanced":{"type":"integer"},
                "resolvedSteps":{"type":"integer"},
                "totalSteps":{"type":"integer"},
                "sampleCount":{"type":"integer"},
                "generation":{"type":"integer"},
                "converged":{"type":"boolean"},
                "empty":{"type":"boolean"},
                "analysisMilliseconds":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["studyId","advanced","resolvedSteps","totalSteps","converged","empty"]
        })json" },
    { "SunStudyState", &MakeRegisteredNativeCommand<SunStudyStateCommand>, false,
      R"json({
            "type":"object",
            "properties":{"studyId":{"type":"string"}},
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyIds":{"type":"array","items":{"type":"string"}},
                "studyCount":{"type":"integer"},
                "studyId":{"type":"string"},
                "live":{"type":"boolean"},
                "resolvedSteps":{"type":"integer"},
                "totalSteps":{"type":"integer"},
                "sampleCount":{"type":"integer"},
                "generation":{"type":"integer"},
                "converged":{"type":"boolean"},
                "empty":{"type":"boolean"},
                "timestep":{"type":"integer"},
                "year":{"type":"integer"},
                "month":{"type":"integer"},
                "day":{"type":"integer"},
                "hourFrom":{"type":"integer"},
                "hourTo":{"type":"integer"},
                "minAltitudeDeg":{"type":"number"},
                "grid":{"type":"number"},
                "groundPad":{"type":"number"},
                "sourceStepCount":{"type":"integer"},
                "analysisMilliseconds":{"type":"number"}
            },
            "additionalProperties":false,
            "required":["studyIds","studyCount","studyId","live"]
        })json" },
    { "GetSunStudyResults", &MakeRegisteredNativeCommand<GetSunStudyResultsCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "includePositions":{"type":"boolean"},
                "includeSteps":{"type":"boolean"},
                "includeAtlas":{"type":"boolean"},
                "packed":{"type":"boolean"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "hours":{"type":"array","items":{"type":"number"}},
                "positions":{"type":"array","items":{"type":"number"}},
                "normals":{"type":"array","items":{"type":"number"}},
                "stepBits":{"type":"array","items":{"type":"integer"}},
                "stepBitsPacked":{"type":"string"},
                "atlasWidth":{"type":"integer"},
                "atlasHeight":{"type":"integer"},
                "atlasPacked":{"type":"string"},
                "atlasDomain":{"type":"string"},
                "atlasReason":{"type":"string"},
                "positionsPacked":{"type":"string"},
                "normalsPacked":{"type":"string"},
                "stepStride":{"type":"integer"},
                "count":{"type":"integer"},
                "resolvedSteps":{"type":"integer"},
                "totalSteps":{"type":"integer"},
                "sampleCount":{"type":"integer"},
                "generation":{"type":"integer"},
                "converged":{"type":"boolean"},
                "empty":{"type":"boolean"}
            },
            "additionalProperties":false,
            "required":["studyId","hours","count","converged","empty"]
        })json" },
    { "CancelSunStudy", &MakeRegisteredNativeCommand<CancelSunStudyCommand>, false,
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "all":{"type":"boolean"}
            },
            "additionalProperties":false
        })json",
      R"json({
            "type":"object",
            "properties":{
                "studyId":{"type":"string"},
                "erased":{"type":"integer"}
            },
            "additionalProperties":false,
            "required":["erased"]
        })json" },
};

} // namespace

NativeCommandResult PrepareCapturedSunStudy (const std::shared_ptr<const CapturedSunStudyInputs>& captured,
                                             const std::atomic<bool>& cancelled,
                                             std::shared_ptr<const evp::sunstudy::StudyRecord> reuseSource)
{
    if (captured == nullptr || captured->snapshot == nullptr)
        return NativeCommandResult::Failure ("sun study preparation requires owned inputs");
    GS::ProcessControlInterruptDelegate processControl (nullptr);
    return StartSunStudyCommand (captured, &cancelled, std::move (reuseSource))
        .ExecuteNative (captured->params, processControl);
}

NativeCommandRegistrations GetSunStudyCommandRegistrations ()
{
    return MakeRegistrationView (kSunStudyRegistrations);
}

} // namespace geomsrv
