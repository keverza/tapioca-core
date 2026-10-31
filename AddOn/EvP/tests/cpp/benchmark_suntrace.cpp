// Opt-in sun-trace measurement (#97 P0). No production caller or timing
// assertions in CI.
//
// It separates the variables the live log mixes: TRIANGLE COUNT (the same
// synthetic block city at several sizes), THREAD COUNT (maxParallel 1/2/4/8/0
// through the real RunCpuTraversal) and the INDEX SPLIT (one index over the
// whole scene vs the follower's analysis + context pair, where a clear ray
// walks both). The ray count is held fixed across sizes, so microseconds per
// ray compare directly.
//
// ⚠️ SYNTHETIC GEOMETRY MEASURES, IT DOES NOT VALIDATE. A block city has none
// of a real model's long slivers, overlapping layers or terrain; a size trend
// that it shows and the live log does not is a finding about the model, not a
// fix. The live sweep goes through the advance command's maxParallel.
//
//   TapiocaSunTraceBench [--sizes 1000,10000,...] [--threads 1,2,4,8,0] [--rays N]
//   TapiocaSunTraceBench --snapshot model.msgpack   (a real model, see LoadDump)
//   TapiocaSunTraceBench --sizes N --write-city city.msgpack   (a dump to check it)
#include "Geometry/MeshSerializer.hpp"
#include "Geometry/QueryEngine.hpp"
#include "MeshFixtures.hpp"
#include "SnapshotMsgpack.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyOccluders.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using geomsrv::QueryEngine;
using geomsrv::Snapshot;

std::vector<size_t> ParseList (const char* text)
{
    std::vector<size_t> values;
    std::stringstream stream (text);
    std::string item;
    while (std::getline (stream, item, ','))
        values.push_back (size_t (std::strtoull (item.c_str (), nullptr, 10)));
    return values;
}

// Deterministic, so two runs build the same city.
struct Lcg {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    double Next ()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return double (state >> 11) * (1.0 / 9007199254740992.0);
    }
};

struct City {
    std::shared_ptr<Snapshot> all;
    std::shared_ptr<Snapshot> analysis; // the central quarter of the blocks
    std::shared_ptr<Snapshot> context;  // every other block and the ground
    std::vector<double> origins;        // sample points, 3 per sample
    std::vector<double> normals;
};

// A square grid of boxes, 12 triangles each, 1 m cells with 0.6 m footprints
// and 1-10 m heights, on a ground slab. Samples sit on the roofs, the four
// walls and the ground between blocks, like the analysis grid on a real one.
City MakeCity (size_t triangles)
{
    City city;
    city.all = std::make_shared<Snapshot> ();
    city.analysis = std::make_shared<Snapshot> ();
    city.context = std::make_shared<Snapshot> ();
    city.all->id = city.analysis->id = city.context->id = 1;
    const size_t boxes = (std::max) (size_t (1), triangles / 12);
    const size_t side = size_t (std::ceil (std::sqrt (double (boxes))));
    const double extent = double (side);
    Lcg random;
    const auto ground = evptest::MakeBox ("ground", -1.0, -1.0, -0.2, extent + 2.0, extent + 2.0, 0.2);
    city.all->meshes.push_back (ground);
    city.context->meshes.push_back (ground);
    const double lo = 0.375 * extent, hi = 0.625 * extent;
    for (size_t b = 0; b < boxes; ++b) {
        const double x = double (b % side) + 0.2, y = double (b / side) + 0.2;
        const double height = 1.0 + 9.0 * random.Next ();
        auto mesh = evptest::MakeBox ("block-" + std::to_string (b), x, y, 0.0, 0.6, 0.6, height);
        const bool central = x >= lo && x < hi && y >= lo && y < hi;
        (central ? city.analysis : city.context)->meshes.push_back (mesh);
        city.all->meshes.push_back (std::move (mesh));
        const double cx = x + 0.3, cy = y + 0.3, mid = 0.5 * height;
        const double points[6][6] = {
            { cx, cy, height, 0, 0, 1 },   { x + 0.6, cy, mid, 1, 0, 0 }, { x, cy, mid, -1, 0, 0 },
            { cx, y + 0.6, mid, 0, 1, 0 }, { cx, y, mid, 0, -1, 0 },      { x - 0.2, y - 0.2, 0.0, 0, 0, 1 },
        };
        for (const auto& point : points) {
            city.origins.insert (city.origins.end (), point, point + 3);
            city.normals.insert (city.normals.end (), point + 3, point + 6);
        }
    }
    return city;
}

struct Sun {
    const char* name;
    double direction[3];
};

// Towards the sun, unit length: a low morning, a high noon, a low evening.
std::vector<Sun> Suns ()
{
    const auto make = [] (const char* name, double azimuthDeg, double elevationDeg) {
        const double az = azimuthDeg * 3.14159265358979 / 180.0, el = elevationDeg * 3.14159265358979 / 180.0;
        return Sun { name, { std::sin (az) * std::cos (el), std::cos (az) * std::cos (el), std::sin (el) } };
    };
    return { make ("morning", 100.0, 15.0), make ("noon", 180.0, 60.0), make ("evening", 260.0, 20.0) };
}

// The accumulator's compaction: only samples facing the sun are traced. The
// list is then cycled to `rays`, so every size traces the same number.
std::vector<double> FrontFacing (const City& city, const Sun& sun, size_t rays)
{
    std::vector<double> facing;
    for (size_t s = 0; s * 3 < city.normals.size (); ++s) {
        const double* n = &city.normals[s * 3];
        if (n[0] * sun.direction[0] + n[1] * sun.direction[1] + n[2] * sun.direction[2] > 0.0)
            facing.insert (facing.end (), &city.origins[s * 3], &city.origins[s * 3] + 3);
    }
    std::vector<double> out;
    out.reserve (rays * 3);
    for (size_t r = 0; r < rays && !facing.empty (); ++r) {
        const size_t s = (r * 7919) % (facing.size () / 3); // a stride, not the surface order
        out.insert (out.end (), &facing[s * 3], &facing[s * 3] + 3);
    }
    return out;
}

struct Measured {
    double ms = 0.0;
    size_t rays = 0;
    size_t hits = 0;
};

Measured Trace (const evp::sunstudy::SunStudyPartitionTraversal& traversal,
                const std::vector<std::vector<double>>& rays, const std::vector<Sun>& suns, size_t maxParallel)
{
    Measured best;
    // The best of up to three passes, stopping once a pass is long enough to
    // be stable; the slow single-thread cases are not run three times.
    for (int pass = 0; pass < 3; ++pass) {
        Measured run;
        const auto started = std::chrono::steady_clock::now ();
        for (size_t k = 0; k < suns.size (); ++k) {
            const size_t count = rays[k].size () / 3;
            std::vector<uint8_t> occluded (count, 0);
            traversal.OccludeDirectional (rays[k].data (), count, suns[k].direction, 0.001, 0.0, occluded.data (),
                                          maxParallel);
            run.rays += count;
            run.hits += size_t (std::count (occluded.begin (), occluded.end (), uint8_t (1)));
        }
        run.ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - started).count ();
        if (pass == 0 || run.ms < best.ms)
            best = run;
        if (run.ms > 2000.0)
            break;
    }
    return best;
}

double Seconds (std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now () - since).count ();
}

// One triangle's corner and two edges.
struct Face {
    const double* a;
    double u[3];
    double v[3];
    double n[3]; // u x v, twice the area
};

Face FaceOf (const geomsrv::Mesh& mesh, size_t t)
{
    Face f {};
    f.a = &mesh.vertices[3 * size_t (mesh.triangles[t])];
    const double* b = &mesh.vertices[3 * size_t (mesh.triangles[t + 1])];
    const double* c = &mesh.vertices[3 * size_t (mesh.triangles[t + 2])];
    for (int k = 0; k < 3; ++k) {
        f.u[k] = b[k] - f.a[k];
        f.v[k] = c[k] - f.a[k];
    }
    f.n[0] = f.u[1] * f.v[2] - f.u[2] * f.v[1];
    f.n[1] = f.u[2] * f.v[0] - f.u[0] * f.v[2];
    f.n[2] = f.u[0] * f.v[1] - f.u[1] * f.v[0];
    return f;
}

// A real model, from the add-on's GET /meshes (curl -o model.msgpack). The dump
// carries no roles, so the whole model is one index, which is what a study
// with no context elements traces. The samples are area-weighted points on its
// triangles with the geometric normal, standing in for the analysis grid.
City LoadDump (const std::string& path, size_t samples)
{
    std::ifstream file (path, std::ios::binary);
    if (!file)
        throw std::runtime_error ("cannot open " + path);
    const std::string bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char> ());
    evptest::ParsedSnapshot parsed = evptest::ParseSnapshot (bytes);
    City city;
    city.all = std::make_shared<Snapshot> ();
    city.all->id = parsed.id;
    city.all->scope = parsed.scope;
    city.all->meshes = std::move (parsed.meshes);
    std::vector<double> cumulative;
    std::vector<std::pair<size_t, size_t>> faces;
    double total = 0.0;
    for (size_t m = 0; m < city.all->meshes.size (); ++m) {
        auto& mesh = city.all->meshes[m];
        evptest::RecomputeBounds (mesh);
        for (size_t t = 0; t + 2 < mesh.triangles.size (); t += 3) {
            const Face f = FaceOf (mesh, t);
            const double area = 0.5 * std::sqrt (f.n[0] * f.n[0] + f.n[1] * f.n[1] + f.n[2] * f.n[2]);
            if (!(area > 0.0) || !std::isfinite (area))
                continue;
            total += area;
            cumulative.push_back (total);
            faces.emplace_back (m, t);
        }
    }
    Lcg random;
    for (size_t s = 0; s < samples && !faces.empty (); ++s) {
        const size_t pick =
            (std::min) (faces.size () - 1,
                        size_t (std::lower_bound (cumulative.begin (), cumulative.end (), random.Next () * total) -
                                cumulative.begin ()));
        const Face f = FaceOf (city.all->meshes[faces[pick].first], faces[pick].second);
        double r1 = random.Next (), r2 = random.Next ();
        if (r1 + r2 > 1.0) {
            r1 = 1.0 - r1;
            r2 = 1.0 - r2;
        }
        const double length = std::sqrt (f.n[0] * f.n[0] + f.n[1] * f.n[1] + f.n[2] * f.n[2]);
        for (int k = 0; k < 3; ++k) {
            city.origins.push_back (f.a[k] + r1 * f.u[k] + r2 * f.v[k]);
            city.normals.push_back (f.n[k] / length);
        }
    }
    return city;
}

void Report (const char* index, const QueryEngine& all, const QueryEngine& first,
             const evp::sunstudy::SunStudyPartitionTraversal& traversal, const std::vector<std::vector<double>>& rays,
             const std::vector<Sun>& suns, const std::vector<size_t>& threads)
{
    const size_t perSun = rays.empty () ? 0 : rays[0].size () / 3;
    for (const size_t maxParallel : threads) {
        const Measured m = Trace (traversal, rays, suns, maxParallel);
        const double usPerRay = m.rays == 0 ? 0.0 : 1000.0 * m.ms / double (m.rays);
        std::printf ("%-8s %-7zu %9zu %9zu %6zu %4zu %4zu %10zu %6.1f%% %9.3f %10.3f\n", index, all.TriangleCount (),
                     first.TriangleCount (), first.BvhNodeCount (), first.BvhDepth (), maxParallel,
                     evp::sunstudy::ChooseThreadCount (perSun, maxParallel), m.rays,
                     m.rays == 0 ? 0.0 : 100.0 * double (m.hits) / double (m.rays), usPerRay,
                     usPerRay > 0.0 ? 1.0 / usPerRay : 0.0);
        std::fflush (stdout);
    }
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<size_t> sizes = { 1000, 10000, 100000, 650000 };
    std::vector<size_t> threads = { 1, 2, 4, 8, 0 };
    size_t rayCount = 20000;
    std::string dump, written;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--sizes")
            sizes = ParseList (argv[i + 1]);
        else if (flag == "--threads")
            threads = ParseList (argv[i + 1]);
        else if (flag == "--rays")
            rayCount = size_t (std::strtoull (argv[i + 1], nullptr, 10));
        else if (flag == "--snapshot")
            dump = argv[i + 1];
        else if (flag == "--write-city")
            written = argv[i + 1];
        else {
            std::fprintf (stderr, "unknown flag %s\n", flag.c_str ());
            return 2;
        }
    }
    std::printf ("config=%s hardwareThreads=%u raysPerSun=%zu suns=3\n", TAPIOCA_BENCH_CONFIG,
                 std::thread::hardware_concurrency (), rayCount);
    std::printf ("%-8s %-7s %9s %9s %6s %4s %4s %10s %7s %9s %10s\n", "index", "tris", "anTris", "bvhNodes", "depth",
                 "mp", "thr", "rays", "hit%", "us/ray", "Mrays/s");
    const auto suns = Suns ();
    const auto raysFor = [&suns, rayCount] (const City& city) {
        std::vector<std::vector<double>> rays;
        for (const auto& sun : suns)
            rays.push_back (FrontFacing (city, sun, rayCount));
        return rays;
    };
    // The first --sizes city in the /meshes format, so --snapshot can be checked
    // against the synthetic run without Archicad: the same scene, the same cost.
    if (!written.empty ()) {
        const City city = MakeCity (sizes.empty () ? 1000 : sizes[0]);
        std::ofstream (written, std::ios::binary) << geomsrv::SerializeSnapshot (*city.all);
        std::printf ("# wrote %zu meshes to %s\n", city.all->meshes.size (), written.c_str ());
        return 0;
    }
    if (!dump.empty ()) {
        const auto builtAt = std::chrono::steady_clock::now ();
        City model;
        try {
            model = LoadDump (dump, 4 * rayCount);
        }
        catch (const std::exception& error) {
            std::fprintf (stderr, "%s: %s\n", dump.c_str (), error.what ());
            return 1;
        }
        const auto all = std::make_shared<const QueryEngine> (model.all);
        std::printf ("# %s: %zu meshes, %zu triangles, loaded and indexed in %.2f s\n", dump.c_str (),
                     model.all->meshes.size (), all->TriangleCount (), Seconds (builtAt));
        Report ("model", *all, *all, evp::sunstudy::SunStudyPartitionTraversal (all, nullptr), raysFor (model), suns,
                threads);
        return 0;
    }
    for (const size_t size : sizes) {
        const auto builtAt = std::chrono::steady_clock::now ();
        const City city = MakeCity (size);
        auto all = std::make_shared<const QueryEngine> (city.all);
        auto analysis = std::make_shared<const QueryEngine> (city.analysis);
        auto context = std::make_shared<const QueryEngine> (city.context);
        std::printf ("# %zu triangles: city and three indexes built in %.2f s\n", all->TriangleCount (),
                     Seconds (builtAt));
        const auto rays = raysFor (city);
        Report ("single", *all, *all, evp::sunstudy::SunStudyPartitionTraversal (all, nullptr), rays, suns, threads);
        Report ("split", *all, *analysis, evp::sunstudy::SunStudyPartitionTraversal (analysis, context), rays, suns,
                threads);
    }
    return 0;
}
