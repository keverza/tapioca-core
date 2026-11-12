// SolarCutReplay -- a diagnostic, not a test: replays a scene exported from the
// host through a real patch-domain sun study and the solar cut, point by
// point, and prints what each stage made and why a stage came back empty or
// failed. Skipped unless TAPIOCA_SOLARCUT_REPLAY names a mesh text file:
//
//   mesh <guid> <vertexCount> <triangleCount>
//   x y z            (one line per vertex, metres)
//   a b c            (one line per triangle)
//
// Optional: TAPIOCA_SOLARCUT_REPLAY_POINTS = "x,y,z;x,y,z" (the nearest study
// cell to each is cut; default: a spread of cells), _SLICE = "from-to" in clock
// minutes (default 690-810), _LAT/_LON/_TZ for the sun (default 50.0, 15.0,
// UTC+1 on 21 March), _SPACING (default 1.0).

#include "ArchViz/SunStudyObstructionController.hpp"

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/SunStudyOccluders.hpp"
#include "SunStudy/SunStudyReuse.hpp"
#include "SunStudy/SunStudyRoles.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "SunStudy/SunStudySurfaceSampling.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using namespace geomsrv::archviz;
using namespace evp::sunstudy;

namespace {

std::string Env (const char* name)
{
    const char* value = std::getenv (name);
    return value != nullptr ? value : "";
}

double EnvNumber (const char* name, double fallback)
{
    const std::string text = Env (name);
    return text.empty () ? fallback : std::atof (text.c_str ());
}

bool ReadMeshes (const std::string& path, geomsrv::Snapshot& snapshot)
{
    std::ifstream in (path);
    std::string word;
    while (in >> word) {
        if (word != "mesh")
            return false;
        geomsrv::Mesh mesh;
        size_t vertices = 0, triangles = 0;
        in >> mesh.guid >> vertices >> triangles;
        mesh.guid = "{" + mesh.guid + "}";
        mesh.vertices.resize (3 * vertices);
        for (double& v : mesh.vertices)
            in >> v;
        mesh.triangles.resize (3 * triangles);
        for (uint32_t& t : mesh.triangles)
            in >> t;
        mesh.triMaterial.assign (triangles, 0);
        for (size_t v = 0; v + 2 < mesh.vertices.size (); v += 3)
            mesh.bounds.Expand (mesh.vertices[v], mesh.vertices[v + 1], mesh.vertices[v + 2]);
        snapshot.meshes.push_back (std::move (mesh));
    }
    return !snapshot.meshes.empty ();
}

// NOAA's solar position, good to a fraction of a degree: enough to reproduce
// the shape of a day, not the host's exact suns.
SunSeries Day (double latitude, double longitude, double zone, int timestep)
{
    constexpr double kPi = 3.14159265358979323846, kDeg = kPi / 180.0;
    const int dayOfYear = 80; // 21 March
    std::vector<SunStep> steps;
    for (int minute = 0; minute < 1440; minute += timestep) {
        const double gamma = 2.0 * kPi / 365.0 * (dayOfYear - 1 + (minute / 60.0 - 12.0) / 24.0);
        const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos (gamma) - 0.032077 * std::sin (gamma) -
                                        0.014615 * std::cos (2 * gamma) - 0.040849 * std::sin (2 * gamma));
        const double decl = 0.006918 - 0.399912 * std::cos (gamma) + 0.070257 * std::sin (gamma) -
                            0.006758 * std::cos (2 * gamma) + 0.000907 * std::sin (2 * gamma) -
                            0.002697 * std::cos (3 * gamma) + 0.00148 * std::sin (3 * gamma);
        const double solar = minute + eqtime + 4.0 * longitude - 60.0 * zone;
        const double hourAngle = (solar / 4.0 - 180.0) * kDeg, lat = latitude * kDeg;
        const double cosZenith =
            std::sin (lat) * std::sin (decl) + std::cos (lat) * std::cos (decl) * std::cos (hourAngle);
        const double altitude = std::asin (std::clamp (cosZenith, -1.0, 1.0));
        // Azimuth from north, clockwise; x east, y north.
        const double azimuth = std::atan2 (std::sin (hourAngle),
                                           std::cos (hourAngle) * std::sin (lat) - std::tan (decl) * std::cos (lat)) +
                               kPi;
        SunStep step;
        step.time = { minute / 60, minute % 60 };
        step.altitudeDegrees = altitude / kDeg;
        step.direction[0] = std::cos (altitude) * std::sin (azimuth);
        step.direction[1] = std::cos (altitude) * std::cos (azimuth);
        step.direction[2] = std::sin (altitude);
        steps.push_back (step);
    }
    return SunSeries::FromSteps (steps, timestep);
}

struct StoreReset {
    ~StoreReset ()
    {
        SunStudyStore::Get ().Clear ();
    }
};

} // namespace

TEST (SolarCutReplay, FromEnvironment)
{
    const std::string path = Env ("TAPIOCA_SOLARCUT_REPLAY");
    if (path.empty ())
        GTEST_SKIP () << "Set TAPIOCA_SOLARCUT_REPLAY to a mesh text file.";
    StoreReset reset;
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 901;
    snapshot->captureStamp = 902;
    ASSERT_TRUE (ReadMeshes (path, *snapshot)) << path;

    const int timestep = 15;
    const double spacing = EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_SPACING", 1.0);
    auto record = std::make_unique<StudyRecord> ();
    record->snapshot = snapshot;
    record->snapshotId = snapshot->id;
    record->series =
        Day (EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_LAT", 50.0), EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_LON", 15.0),
             EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_TZ", 1.0), timestep);
    record->timestepMinutes = timestep;
    record->hourFrom = 0;
    record->hourTo = 24;
    record->gridSpacing = spacing;
    const auto roles = ResolveElementRoles (*snapshot, {}, {}, {});
    for (const auto role : roles.roles)
        record->elementRoles.push_back (uint8_t (role));
    SurfaceSamplingOptions options;
    options.domain = SamplingDomain::SurfacePatch;
    options.spacing = spacing;
    options.normalOffset = 0.10;
    auto sampling = BuildSurfaceSampling (*snapshot, roles.SampleMask (), options);
    ASSERT_TRUE (sampling.valid);
    record->samplingLayout = std::move (sampling.layout);
    record->domain = SamplingDomain::SurfacePatch;
    record->positions = sampling.patches.positions;
    record->normals = sampling.patches.normals;
    record->patchAtlas.Fit (sampling.patches, 1, 8192);
    record->patchGrid = std::move (sampling.patches);
    record->occluders = BuildSunStudyOccluders (*snapshot, roles);
    record->traversal =
        std::make_shared<SunStudyPartitionTraversal> (record->occluders->analysis, record->occluders->context);
    const StudyInputs inputs { snapshot->id, record->series.Version (),
                               record->patchGrid.Count () * 73856093ull ^ record->patchGrid.spans.size () };
    std::string error;
    ASSERT_TRUE (AttachRustStudyCore (*record, *snapshot, nullptr, 8192, error)) << error;
    SyncStudy (*record, inputs);
    SetSampleMeshes (*record);
    const size_t samples = record->positions.size () / 3;
    const std::vector<double> positions = record->positions;
    auto& store = SunStudyStore::Get ();
    const std::string id = store.Insert (std::move (record));
    ASSERT_FALSE (id.empty ());
    size_t advanced = 0;
    ASSERT_TRUE (store.Advance (id, 100000, 4, 0.001, 0.0, advanced, error)) << error;
    std::cout << "study: " << snapshot->meshes.size () << " meshes, " << samples << " cells, " << advanced
              << " steps\n";

    // The points: the nearest cell to each given one, or a spread.
    std::vector<size_t> cells;
    const auto nearest = [&] (double x, double y, double z) {
        size_t best = 0;
        double bestD = INFINITY;
        for (size_t s = 0; s < samples; ++s) {
            const double dx = positions[3 * s] - x, dy = positions[3 * s + 1] - y, dz = positions[3 * s + 2] - z;
            if (dx * dx + dy * dy + dz * dz < bestD)
                bestD = dx * dx + dy * dy + dz * dz, best = s;
        }
        return best;
    };
    if (const std::string text = Env ("TAPIOCA_SOLARCUT_REPLAY_POINTS"); !text.empty ()) {
        std::stringstream list (text);
        std::string item;
        while (std::getline (list, item, ';')) {
            double p[3] = {};
            if (std::sscanf (item.c_str (), "%lf,%lf,%lf", &p[0], &p[1], &p[2]) == 3)
                cells.push_back (nearest (p[0], p[1], p[2]));
        }
    }
    else {
        const size_t count = size_t (EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_COUNT", 60));
        for (size_t i = 0; i < count; ++i)
            cells.push_back ((i * 7919 + 13) % samples);
    }
    int from = 690, to = 810;
    if (const std::string slice = Env ("TAPIOCA_SOLARCUT_REPLAY_SLICE"); !slice.empty ())
        std::sscanf (slice.c_str (), "%d-%d", &from, &to);

    SunObstructionController controller;
    size_t failed = 0;
    for (const size_t cell : cells) {
        SunObstructionRequest request;
        request.studyId = id;
        request.samples = { cell };
        request.mode = SunObstructionMode::SolarCut;
        request.fromMinute = from;
        request.toMinute = to;
        std::shared_ptr<const SunObstructionFrame> frame, seen;
        const auto started = std::chrono::steady_clock::now ();
        const double limit = EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_TIMEOUT", 60.0);
        bool finished = false;
        while (std::chrono::steady_clock::now () - started < std::chrono::duration<double> (limit)) {
            controller.Request (request);
            frame = controller.Latest ();
            if (frame != nullptr)
                seen = frame;
            if (frame != nullptr && frame->cutStage == SunSolarCutStage::Done && !controller.Busy ()) {
                finished = true;
                break;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        if (!finished) {
            // A stall: where the worker is, whether it still ticks, and out --
            // a worker that never polls its cancellation would hang the exit.
            std::cout << "STALL at cell " << cell << " (" << positions[3 * cell] << ", " << positions[3 * cell + 1]
                      << ", " << positions[3 * cell + 2] << ")";
            if (seen != nullptr && seen->progress != nullptr) {
                const auto& progress = *seen->progress;
                const uint64_t ticks = progress.ticks.load ();
                std::this_thread::sleep_for (std::chrono::seconds (2));
                std::cout << ": phase " << SunSolarCutPhaseName (SunSolarCutPhase (progress.phase.load ())) << ", "
                          << progress.done.load () << "/" << progress.total.load () << ", ticks " << ticks << " -> "
                          << progress.ticks.load () << " in 2 s, removed " << seen->removedVolume << " m3";
            }
            std::cout << std::endl;
            std::_Exit (3);
        }
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count ();
        const auto& e = frame->exposure;
        std::cout << "cell " << cell << " (" << positions[3 * cell] << ", " << positions[3 * cell + 1] << ", "
                  << positions[3 * cell + 2] << ") " << seconds << " s: removed " << frame->removedVolume << " m3 of "
                  << frame->cutObjects.size () << ", won " << frame->wonCells << ", cells " << frame->cutCells.cells
                  << " (" << frame->cutCells.area << " m2)"
                  << ", lit triangles " << e.triangles.size () << ", surfaces " << e.surfacesDone << "/"
                  << e.surfacesShaded << ", skipped " << e.skipped << ", work " << e.work;
        if (!frame->error.empty ())
            std::cout << "\n   CUT ERROR: " << frame->error;
        if (!e.error.empty ())
            std::cout << "\n   LIT ERROR: " << e.error;
        if (!e.note.empty ())
            std::cout << "\n   LIT NOTE: " << e.note;
        if (!frame->cutCells.error.empty ())
            std::cout << "\n   CELLS ERROR: " << frame->cutCells.error;
        // How much each changed cell gains, in steps: a speckle of one-step
        // gains is a thin per-step strip sampled at cell centres.
        if (!frame->cutCells.samples.empty ()) {
            std::map<int, size_t> gains;
            for (size_t i = 0; i < frame->cutCells.samples.size (); ++i)
                ++gains[int (std::lround ((frame->cutCells.after[i] - frame->cutCells.before[i]) * 60.0 / timestep))];
            std::cout << "\n   steps gained per cell:";
            for (const auto& [steps, count] : gains)
                std::cout << ' ' << steps << "x" << count;
        }
        std::cout << '\n';
        failed += !e.error.empty () || !e.note.empty () ? 1 : 0;
    }
    controller.Request ({});
    std::cout << failed << " of " << cells.size () << " points with a failed or incomplete lit surface\n";
}
