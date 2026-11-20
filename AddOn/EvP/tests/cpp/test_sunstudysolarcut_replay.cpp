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

// The replay study: a patch-domain day study over the scene, advanced to the
// end, in the store. Empty on failure, with `error` when there is one.
std::string ReplayStudy (const std::shared_ptr<geomsrv::Snapshot>& snapshot, int timestep, double spacing,
                         size_t& samples, std::vector<double>& positions, size_t& advanced, std::string& error)
{
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
    if (!sampling.valid)
        return {};
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
    if (!AttachRustStudyCore (*record, *snapshot, nullptr, 8192, error))
        return {};
    SyncStudy (*record, inputs);
    SetSampleMeshes (*record);
    samples = record->positions.size () / 3;
    positions = record->positions;
    auto& store = SunStudyStore::Get ();
    const std::string id = store.Insert (std::move (record));
    if (id.empty ())
        return {};
    advanced = 0;
    if (!store.Advance (id, 100000, 4, 0.001, 0.0, advanced, error))
        return {};
    return id;
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
    size_t samples = 0, advanced = 0;
    std::vector<double> positions;
    std::string error;
    const std::string id = ReplayStudy (snapshot, timestep, spacing, samples, positions, advanced, error);
    ASSERT_FALSE (id.empty ()) << error;
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
                  << " (" << frame->cutCells.area << " m2, " << frame->cutCells.outline.size () << " outline edges)"
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

// Does every won-lit triangle the cut draws really see the sun? For each step's
// won region (SolarExposureResult::steps), four interior points of every
// triangle cast a ray toward that step's sun, once through the scene as it is
// and once through the scene as cut (each cut element replaced by what of it
// remains, from the same ComputeSolarCut the controller runs). A won triangle
// must be dark before and clear after. Printed per point: the won area that is
// still dark after the cut ("phantom": lit where no sun reaches), the area that
// was already lit, and the worst phantom triangles. Skipped like FromEnvironment.
TEST (SolarCutReplay, WonLitSurfaceSeesTheSun)
{
    const std::string path = Env ("TAPIOCA_SOLARCUT_REPLAY");
    if (path.empty ())
        GTEST_SKIP () << "Set TAPIOCA_SOLARCUT_REPLAY to a mesh text file.";
    StoreReset reset;
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 903;
    snapshot->captureStamp = 904;
    ASSERT_TRUE (ReadMeshes (path, *snapshot)) << path;
    const int timestep = 15;
    const double spacing = EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_SPACING", 1.0);
    size_t samples = 0, advanced = 0;
    std::vector<double> positions;
    std::string error;
    const std::string id = ReplayStudy (snapshot, timestep, spacing, samples, positions, advanced, error);
    ASSERT_FALSE (id.empty ()) << error;
    const auto record = SunStudyStore::Get ().CompletedRecord (id);
    ASSERT_NE (record, nullptr);

    std::vector<size_t> cells;
    if (const std::string text = Env ("TAPIOCA_SOLARCUT_REPLAY_CELLS"); !text.empty ()) {
        std::stringstream list (text);
        std::string item;
        while (std::getline (list, item, ','))
            cells.push_back (size_t (std::atoll (item.c_str ())));
    }
    else
        for (size_t i = 0, count = size_t (EnvNumber ("TAPIOCA_SOLARCUT_REPLAY_COUNT", 60)); i < count; ++i)
            cells.push_back ((i * 7919 + 13) % samples);
    int from = 690, to = 810;
    if (const std::string slice = Env ("TAPIOCA_SOLARCUT_REPLAY_SLICE"); !slice.empty ())
        std::sscanf (slice.c_str (), "%d-%d", &from, &to);

    const geomsrv::QueryEngine before (snapshot);
    SunObstructionController controller;
    double phantomTotal = 0.0, wonTotal = 0.0;
    double sweptPhantomTotal = 0.0, sweptTotal = 0.0;
    for (const size_t cell : cells) {
        SunObstructionRequest request;
        request.studyId = id;
        request.samples = { cell };
        request.mode = SunObstructionMode::SolarCut;
        request.fromMinute = from;
        request.toMinute = to;
        std::shared_ptr<const SunObstructionFrame> frame;
        const auto started = std::chrono::steady_clock::now ();
        while (std::chrono::steady_clock::now () - started < std::chrono::seconds (60)) {
            controller.Request (request);
            frame = controller.Latest ();
            if (frame != nullptr && frame->cutStage == SunSolarCutStage::Done && !controller.Busy ())
                break;
            frame = nullptr;
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        ASSERT_NE (frame, nullptr) << "cell " << cell << " did not finish";
        if (frame->exposure.steps.empty ())
            continue;

        // The cut again, as the controller builds it, for what remains.
        SolarCutInput input;
        std::copy (&record->positions[3 * cell], &record->positions[3 * cell] + 3, input.point);
        input.normal = &record->normals[3 * cell];
        if (cell < record->sampleMeshes.size ())
            input.ownMesh = record->sampleMeshes[cell];
        for (const auto& step : record->series.Steps ()) {
            const int minute = step.time.hour * 60 + step.time.minute;
            if (minute >= from && minute < to)
                input.steps.push_back (step);
        }
        input.hoursPerStep = timestep / 60.0;
        input.blocks.assign (snapshot->meshes.size (), 1);
        const auto lit = [&] (const double direction[3]) {
            const double origin[3] = { input.point[0] + 0.001 * direction[0], input.point[1] + 0.001 * direction[1],
                                       input.point[2] + 0.001 * direction[2] };
            for (const auto& engine : { record->occluders->analysis, record->occluders->context })
                if (engine != nullptr && engine->Raycast (origin, direction, 0.0).hit)
                    return false;
            return true;
        };
        SolarCutResult cut;
        ASSERT_TRUE (ComputeSolarCut (*snapshot, input, cut, lit)) << cut.error;
        auto afterScene = std::make_shared<geomsrv::Snapshot> (*snapshot);
        afterScene->id = snapshot->id + 1000 + cell;
        for (const auto& [mesh, kept] : cut.kept) {
            afterScene->meshes[mesh] = kept;
            auto& bounds = afterScene->meshes[mesh].bounds;
            bounds = geomsrv::Aabb ();
            for (size_t v = 0; v + 2 < kept.vertices.size (); v += 3)
                bounds.Expand (kept.vertices[v], kept.vertices[v + 1], kept.vertices[v + 2]);
        }
        const geomsrv::QueryEngine after (afterScene);

        double phantom = 0.0, already = 0.0, won = 0.0;
        std::vector<std::pair<double, std::string>> worst;
        for (const auto& step : frame->exposure.steps) {
            const SunStep* sun = nullptr;
            for (const auto& candidate : record->series.Steps ())
                if (candidate.time.hour * 60 + candidate.time.minute == step.minute)
                    sun = &candidate;
            if (sun == nullptr)
                continue;
            double dir[3] = { sun->direction[0], sun->direction[1], sun->direction[2] };
            const double length = std::sqrt (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            for (double& c : dir)
                c /= length;
            for (const auto& t : step.triangles) {
                const double e1[3] = { t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2] };
                const double e2[3] = { t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2] };
                const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                      e1[0] * e2[1] - e1[1] * e2[0] };
                const double area = 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!(area > 1e-8))
                    continue;
                won += area;
                const double weights[4][3] = {
                    { 1.0 / 3, 1.0 / 3, 1.0 / 3 }, { 0.6, 0.2, 0.2 }, { 0.2, 0.6, 0.2 }, { 0.2, 0.2, 0.6 }
                };
                int dark = 0, clearBefore = 0;
                double centre[3] = {};
                for (int k = 0; k < 4; ++k) {
                    double origin[3];
                    for (int a = 0; a < 3; ++a)
                        origin[a] = weights[k][0] * t.v[0][a] + weights[k][1] * t.v[1][a] + weights[k][2] * t.v[2][a] +
                                    0.002 * dir[a];
                    if (k == 0)
                        std::copy (origin, origin + 3, centre);
                    dark += after.Raycast (origin, dir, 0.0).hit ? 1 : 0;
                    clearBefore += before.Raycast (origin, dir, 0.0).hit ? 0 : 1;
                }
                if (dark >= 3) {
                    phantom += area;
                    char text[200];
                    std::snprintf (text, sizeof text, "%02d:%02d mesh %u at (%.2f, %.2f, %.2f) %.3f m2",
                                   step.minute / 60, step.minute % 60, t.mesh, centre[0], centre[1], centre[2], area);
                    worst.emplace_back (area, text);
                }
                else if (clearBefore >= 3)
                    already += area;
            }
        }
        std::sort (worst.begin (), worst.end (), [] (const auto& a, const auto& b) { return a.first > b.first; });
        std::cout << "cell " << cell << " (" << positions[3 * cell] << ", " << positions[3 * cell + 1] << ", "
                  << positions[3 * cell + 2] << "): won " << won << " m2 over " << frame->exposure.steps.size ()
                  << " steps, PHANTOM " << phantom << " m2, already lit " << already << " m2\n";
        for (size_t k = 0; k < worst.size () && k < 5; ++k)
            std::cout << "   phantom " << worst[k].second << '\n';
        phantomTotal += phantom;
        wonTotal += won;
        // The swept spans (what the panel draws between steps): a point must be
        // clear after the cut for some sun across its span, the sun taken between
        // the two measured steps that bracket each moment (normalised lerp).
        const auto& day = record->series.Steps ();
        const auto sunAt = [&day] (double minute, double out[3]) {
            for (size_t i = 0; i + 1 < day.size (); ++i) {
                const double a = day[i].time.hour * 60.0 + day[i].time.minute;
                const double b = day[i + 1].time.hour * 60.0 + day[i + 1].time.minute;
                if (minute < a - 1e-9 || minute > b + 1e-9)
                    continue;
                const double f = b > a ? (minute - a) / (b - a) : 0.0;
                double length = 0.0;
                for (int c = 0; c < 3; ++c) {
                    out[c] = day[i].direction[c] + f * (day[i + 1].direction[c] - day[i].direction[c]);
                    length += out[c] * out[c];
                }
                length = std::sqrt (length);
                for (int c = 0; c < 3; ++c)
                    out[c] /= length;
                return true;
            }
            return false;
        };
        double sweptPhantom = 0.0, swept = 0.0;
        std::vector<std::pair<double, std::string>> sweptWorst;
        for (const auto& span : frame->exposure.spans) {
            std::vector<std::array<double, 3>> suns;
            for (int k = 0; k <= 8; ++k) {
                double d[3];
                if (sunAt (span.from + (span.to - span.from) * k / 8.0, d))
                    suns.push_back ({ d[0], d[1], d[2] });
            }
            for (const auto& t : span.triangles) {
                const double e1[3] = { t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2] };
                const double e2[3] = { t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2] };
                const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                      e1[0] * e2[1] - e1[1] * e2[0] };
                const double area = 0.5 * std::sqrt (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (!(area > 1e-8))
                    continue;
                swept += area;
                const double weights[4][3] = {
                    { 1.0 / 3, 1.0 / 3, 1.0 / 3 }, { 0.6, 0.2, 0.2 }, { 0.2, 0.6, 0.2 }, { 0.2, 0.2, 0.6 }
                };
                int never = 0;
                double centre[3] = {};
                for (int k = 0; k < 4; ++k) {
                    double point[3];
                    for (int a = 0; a < 3; ++a)
                        point[a] = weights[k][0] * t.v[0][a] + weights[k][1] * t.v[1][a] + weights[k][2] * t.v[2][a];
                    if (k == 0)
                        std::copy (point, point + 3, centre);
                    bool reached = false;
                    for (const auto& d : suns) {
                        const double origin[3] = { point[0] + 0.002 * d[0], point[1] + 0.002 * d[1],
                                                   point[2] + 0.002 * d[2] };
                        if (!after.Raycast (origin, d.data (), 0.0).hit) {
                            reached = true;
                            break;
                        }
                    }
                    never += reached ? 0 : 1;
                }
                if (never >= 3) {
                    sweptPhantom += area;
                    char text[220];
                    std::snprintf (text, sizeof text, "%.0f-%.0f min mesh %u at (%.2f, %.2f, %.2f) %.3f m2", span.from,
                                   span.to, t.mesh, centre[0], centre[1], centre[2], area);
                    sweptWorst.emplace_back (area, text);
                }
            }
        }
        std::sort (sweptWorst.begin (), sweptWorst.end (),
                   [] (const auto& a, const auto& b) { return a.first > b.first; });
        std::cout << "   swept " << swept << " m2 over " << frame->exposure.spans.size () << " spans, SWEPT PHANTOM "
                  << sweptPhantom << " m2\n";
        for (size_t k = 0; k < sweptWorst.size () && k < 5; ++k)
            std::cout << "   swept phantom " << sweptWorst[k].second << '\n';
        sweptPhantomTotal += sweptPhantom;
        sweptTotal += swept;
        // The re-counted cells: what a changed cell reads after the cut is a
        // whole-day count, so recount it independently -- every day step facing
        // the cell, traced from the cell through the scene as cut -- and say how
        // much of the gain falls outside the slice.
        size_t cellsWrong = 0, cellsOutside = 0;
        double worstCell = 0.0;
        const auto& cc = frame->cutCells;
        for (size_t i = 0; i < cc.samples.size (); ++i) {
            const size_t s = cc.samples[i];
            if (3 * s + 2 >= record->positions.size ())
                continue;
            const double* p = &record->positions[3 * s];
            const double* nrm = &record->normals[3 * s];
            size_t clearAfter = 0, clearBeforeDay = 0, gainedInSlice = 0;
            for (const auto& step : day) {
                double d[3] = { step.direction[0], step.direction[1], step.direction[2] };
                const double length = std::sqrt (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (!(length > 0.0) || d[2] <= 0.0)
                    continue;
                for (double& c : d)
                    c /= length;
                if (nrm[0] * d[0] + nrm[1] * d[1] + nrm[2] * d[2] <= 0.0)
                    continue;
                const double origin[3] = { p[0] + 0.001 * d[0], p[1] + 0.001 * d[1], p[2] + 0.001 * d[2] };
                const bool a = !after.Raycast (origin, d, 0.0).hit, b = !before.Raycast (origin, d, 0.0).hit;
                clearAfter += a ? 1 : 0;
                clearBeforeDay += b ? 1 : 0;
                const int minute = step.time.hour * 60 + step.time.minute;
                gainedInSlice += a && !b && minute >= from && minute < to ? 1 : 0;
            }
            const double hours = timestep / 60.0;
            const double off = std::fabs (clearAfter * hours - cc.after[i]);
            worstCell = (std::max) (worstCell, off);
            cellsWrong += off > hours + 1e-6 ? 1 : 0;
            cellsOutside += gainedInSlice == 0 ? 1 : 0;
            (void) clearBeforeDay;
        }
        std::cout << "   cells " << cc.samples.size () << " changed: " << cellsWrong
                  << " read more than a step off the independent count (worst " << worstCell << " h), " << cellsOutside
                  << " gain nothing inside the slice\n";
    }
    controller.Request ({});
    std::cout << "phantom " << phantomTotal << " of " << wonTotal << " m2 won-lit\n";
    std::cout << "swept phantom " << sweptPhantomTotal << " of " << sweptTotal << " m2 swept\n";
}
