#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "NativeCommands/SunStudyCommands.hpp"

#include "Geometry/MeshStore.hpp"
#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyRaster.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudySampler.hpp"
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

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

GS::UniString Text (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

// The study a verb operates on when it names none. Keeps a console session short
// while the contract stays multi-study underneath.
std::string ReadStudyId (const GS::ObjectState& params)
{
    GS::UniString id;
    if (params.Get ("studyId", id) && !id.IsEmpty ())
        return Utf8 (id);

    const std::vector<std::string> ids = SunStudyStore::Get ().Ids ();
    return ids.empty () ? std::string () : ids.back ();
}

GS::Int32 ReadInt (const GS::ObjectState& params, const char* key, GS::Int32 fallback)
{
    GS::Int32 value = 0;
    return params.Get (key, value) ? value : fallback;
}

double ReadDouble (const GS::ObjectState& params, const char* key, double fallback)
{
    double value = 0.0;
    return params.Get (key, value) ? value : fallback;
}

std::string ReadString (const GS::ObjectState& params, const char* key, const char* fallback)
{
    GS::UniString value;
    if (params.Get (key, value) && !value.IsEmpty ())
        return Utf8 (value);
    return std::string (fallback);
}

// ⚠️ BULK ARRAYS TRAVEL PACKED, AND THIS IS NOT A MICRO-OPTIMISATION. A live
// study of 176,106 samples over 49 timesteps measured 1,209 ms of ANALYSIS
// inside 15,636 ms of call: fourteen of those seconds were the wire, carrying a
// million doubles up as JSON text and 8.6 million step bits back the same way.
// Packed, the same payloads are base64 over raw bytes -- one bit per step
// instead of two characters, eight bytes per coordinate instead of twenty --
// and they parse in one pass instead of eight million allocations.
//
// The plain arrays stay for small studies and for anything reading by eye; a
// caller asks for packed when the size is worth it.
GS::UniString PackDoubles (const std::vector<double>& values)
{
    std::vector<unsigned char> bytes (values.size () * sizeof (double));
    if (!values.empty ())
        std::memcpy (bytes.data (), values.data (), bytes.size ());
    return Base64Encode (bytes);
}

bool UnpackDoubles (const GS::UniString& text, std::vector<double>& values)
{
    std::vector<unsigned char> bytes;
    if (!Base64Decode (text, bytes))
        return false;
    if (bytes.size () % sizeof (double) != 0)
        return false;
    values.resize (bytes.size () / sizeof (double));
    if (!values.empty ())
        std::memcpy (values.data (), bytes.data (), bytes.size ());
    return true;
}

// One BIT per (sample, step), sample-major, LSB first within each byte. A step
// bit is one of two values, so a byte per step wastes seven eighths of the wire.
GS::UniString PackBits (const std::vector<uint8_t>& flags)
{
    std::vector<unsigned char> bytes ((flags.size () + 7) / 8, 0);
    for (size_t i = 0; i < flags.size (); ++i) {
        if (flags[i] != 0)
            bytes[i / 8] |= (unsigned char) (1u << (i % 8));
    }
    return Base64Encode (bytes);
}

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
    GS::String GetName () const override
    {
        return "StartSunStudy";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        std::shared_ptr<const Snapshot> snapshot = MeshStore::Get ().Current ();
        if (snapshot == nullptr)
            return NativeCommandResult::Failure ("no snapshot is live - call Tapioca.BuildSnapshot first");

        std::shared_ptr<const QueryEngine> engine = QueryIndexCache::Get ().For (snapshot);
        if (engine == nullptr)
            return NativeCommandResult::Failure ("the snapshot has no geometry to study");

        API_PlaceInfo place = {};
        const GSErrCode err = ACAPI_GeoLocation_GetPlaceSets (&place);
        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL ("ACAPI_GeoLocation_GetPlaceSets", err, "reading the project's geo location"));
        }

        const GS::Int32 year = ReadInt (params, "year", place.year);
        const GS::Int32 month = ReadInt (params, "month", place.month);
        const GS::Int32 day = ReadInt (params, "day", place.day);
        const GS::Int32 timestep = std::max<GS::Int32> (1, ReadInt (params, "timestep", 60));
        const GS::Int32 hourFrom = ReadInt (params, "hourFrom", 0);
        const GS::Int32 hourTo = ReadInt (params, "hourTo", 24);
        const double minAltitude = ReadDouble (params, "minAltitudeDeg", 0.0);

        // ---- the day's sun, one host call per timestep -----------------------
        //
        // ⚠️ ARCHICAD COMPUTES EVERY ONE OF THESE. A solar formula written here
        // would be a second answer, and a study whose sun disagrees with the
        // model's own shadows is worse than no study.
        std::vector<SunStep> raw;
        for (const evp::sunstudy::TimeOfDay& moment : evp::sunstudy::EnumerateTimesteps (timestep, hourFrom, hourTo)) {
            API_PlaceInfo moment_place = place;
            moment_place.year = (unsigned short) year;
            moment_place.month = (unsigned short) month;
            moment_place.day = (unsigned short) day;
            moment_place.hour = (unsigned short) moment.hour;
            moment_place.minute = (unsigned short) moment.minute;
            moment_place.second = 0;

            if (ACAPI_GeoLocation_CalcSunOnPlace (&moment_place) != NoError)
                continue; // a moment the host could not resolve is dropped, not guessed

            SunStep step;
            step.time = moment;
            step.altitudeDegrees = moment_place.sunAngZ * 180.0 / 3.14159265358979323846;

            // Model space, exactly as GetPlaceInfo reports it: sunAngXY is
            // already measured from +X in the model's own frame, so this is a
            // plain spherical-to-cartesian with no north term.
            const double horizontal = std::cos (moment_place.sunAngZ);
            step.direction[0] = horizontal * std::cos (moment_place.sunAngXY);
            step.direction[1] = horizontal * std::sin (moment_place.sunAngXY);
            step.direction[2] = std::sin (moment_place.sunAngZ);
            raw.push_back (step);
        }

        auto record = std::make_unique<StudyRecord> ();
        record->series = SunSeries::FromSteps (raw, timestep, minAltitude);
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
        bool haveBounds = false;
        for (const Mesh& mesh : snapshot->meshes) {
            for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3) {
                for (int axis = 0; axis < 3; ++axis) {
                    const double value = mesh.vertices[v + axis];
                    if (!haveBounds) {
                        snapshotMin[axis] = snapshotMax[axis] = value;
                    }
                    else {
                        snapshotMin[axis] = std::min (snapshotMin[axis], value);
                        snapshotMax[axis] = std::max (snapshotMax[axis], value);
                    }
                }
                haveBounds = true;
            }
        }
        if (!haveBounds)
            return NativeCommandResult::Failure ("the snapshot has no vertices to bound");

        const double spacing = ReadDouble (params, "grid", 2.0);
        const double pad = ReadDouble (params, "pad", -1.0);
        const double zOffset = ReadDouble (params, "zOffset", 0.10);
        const std::string sampleMode = ReadString (params, "samples", "surfaces");
        const bool sampleSurfaces = (sampleMode == "surfaces");
        const bool sampleExplicit = (sampleMode == "explicit");

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
            std::vector<double> vertices;
            std::vector<uint32_t> triangles;
            std::vector<uint32_t> groups;
            for (size_t m = 0; m < snapshot->meshes.size (); ++m) {
                const Mesh& mesh = snapshot->meshes[m];
                const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
                vertices.insert (vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
                for (uint32_t index : mesh.triangles)
                    triangles.push_back (base + index);
                groups.resize (triangles.size () / 3, static_cast<uint32_t> (m));
            }

            // ⚠️ WINDING IS PROVED BEFORE ANYTHING IS SAMPLED, because every
            // later step trusts the face normal: the sampler lifts each sample
            // ALONG it and the occlusion pass culls on it. An inward-wound
            // element would push its samples into the solid and report its
            // sunlit face as permanently dark -- a plausible study of a building
            // that happens to be in shade.
            evp::sunstudy::WindingReport winding;
            const std::vector<uint32_t> oriented =
                evp::sunstudy::OrientOutward (vertices.data (), vertices.size () / 3, triangles.data (),
                                              triangles.size () / 3, groups.data (), winding);

            evp::sunstudy::SamplerOptions options;
            options.spacing = spacing;
            options.normalOffset = zOffset;
            options.jitter = ReadDouble (params, "jitter", 0.0);
            options.wantLayouts = true;

            const evp::sunstudy::SampleGrid samples =
                evp::sunstudy::BuildSampleGrid (vertices.data (), vertices.size () / 3, oriented.data (),
                                                oriented.size () / 3, groups.data (), options);
            if (!samples.valid) {
                return NativeCommandResult::Failure (
                    "the surface sample grid was refused - the requested spacing would exceed the sample ceiling on "
                    "this model; ask for a coarser grid");
            }

            record->positions = samples.positions;
            record->normals = samples.normals;
            undersizedFaces = samples.undersizedFaces;
            degenerateFaces = samples.degenerateFaces;

            // ⚠️ BUILT ONCE, HERE, BESIDE THE SAMPLES IT DESCRIBES. The packing
            // is a pure function of the sample grid, so rebuilding it per read
            // would produce a different arrangement and silently invalidate
            // every texture coordinate already handed to a consumer.
            record->sampleGrid = samples;
            record->atlas = evp::sunstudy::BuildSunStudyAtlas (samples);
            atlasWidth = record->atlas.width;
            atlasHeight = record->atlas.height;
            atlasFaces = record->atlas.placedFaces;
            closedGroups = winding.closed;
            flippedGroups = winding.flipped;
            gridVersion = static_cast<uint64_t> (samples.Count ()) * 73856093ull ^
                          static_cast<uint64_t> (triangles.size ()) * 19349663ull;
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

        record->gridSpacing = spacing;
        record->groundPad = reportedPad;
        record->traversal = std::make_shared<CpuTraversal> (engine);

        evp::sunstudy::StudyInputs inputs;
        inputs.geometryVersion = engine->SnapshotId ();
        inputs.sunVersion = record->series.Version ();
        inputs.gridVersion = gridVersion;
        record->session.Sync (inputs, record->series, record->Samples ());

        const StudyProgress progress = record->session.Progress ();
        const double daylightHours = record->series.DaylightHours ();
        const size_t sourceSteps = record->sourceStepCount;

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
            if (SunStudyStore::Get ().AtlasImage (id, atlasWidth, atlasHeight, image, atlasError)) {
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

NativeCommandRegistrations GetSunStudyCommandRegistrations ()
{
    return MakeRegistrationView (kSunStudyRegistrations);
}

} // namespace geomsrv
