#include "SunStudy/SunStudyCrownClearance.hpp"

#include "SunStudy/SunStudyLiftedPoint.hpp"
#include "SunStudy/SunStudyOccluders.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <thread>

namespace evp::sunstudy {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Per engine, one mesh mask per host: everything enabled but that host.
struct EngineMasks {
    const geomsrv::QueryEngine* engine = nullptr;
    std::vector<std::vector<uint8_t>> byHost; // empty mask vector: host absent, use `all`
    std::vector<uint8_t> all;
};

EngineMasks MasksFor (const std::shared_ptr<const geomsrv::QueryEngine>& engine,
                      const std::vector<std::string>& hostGuids)
{
    EngineMasks m;
    if (engine == nullptr)
        return m;
    m.engine = engine.get ();
    const size_t meshes = engine->MeshCount ();
    m.all.assign (meshes, 1);
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < meshes; ++i)
        index.emplace (engine->MeshGuid (i), i);
    m.byHost.resize (hostGuids.size ());
    for (size_t g = 0; g < hostGuids.size (); ++g) {
        const auto at = index.find (hostGuids[g]);
        if (at == index.end ())
            continue;
        m.byHost[g] = m.all;
        m.byHost[g][at->second] = 0;
    }
    return m;
}

} // namespace

bool MeasureCrownClearance (const SunStudyOccluders& occluders, const std::vector<double>& x,
                            const std::vector<double>& y, const std::vector<double>& groundZ,
                            const std::vector<uint32_t>& host, const std::vector<std::string>& hostGuids,
                            const CrownClearanceOptions& options, CrownClearance& out, std::string& error,
                            const std::function<bool ()>& isCancelled, std::atomic<size_t>* done)
{
    out = CrownClearance ();
    error.clear ();
    const size_t n = x.size ();
    if (y.size () != n || groundZ.size () != n || host.size () != n) {
        error = "the samples' positions and hosts do not match";
        return false;
    }
    if (options.levels.empty () || !std::is_sorted (options.levels.begin (), options.levels.end ()) ||
        options.levels.front () <= 0.0 || !(options.reach > 0.0) || options.directions < 4) {
        error = "the clearance levels, reach or directions are refused";
        return false;
    }
    const EngineMasks engines[2] = { MasksFor (occluders.analysis, hostGuids),
                                     MasksFor (occluders.context, hostGuids) };
    const size_t k = options.levels.size ();
    out.levels = options.levels;
    out.reach = options.reach;
    out.directions = options.directions;
    out.overhead.assign (n, -1.0f);
    out.free.assign (n * k, float (options.reach));

    std::vector<std::array<double, 3>> fan (size_t (options.directions));
    for (int d = 0; d < options.directions; ++d) {
        const double a = 2.0 * kPi * double (d) / double (options.directions);
        fan[size_t (d)] = { std::cos (a), std::sin (a), 0.0 };
    }

    std::atomic<size_t> next { 0 };
    std::atomic<bool> stop { false };
    auto work = [&] () {
        for (;;) {
            const size_t s = next.fetch_add (1);
            if (s >= n || stop.load ())
                return;
            if (isCancelled && (s & 63u) == 0 && isCancelled ()) {
                stop = true;
                return;
            }
            const double ground[3] = { x[s], y[s], groundZ[s] };
            const double above = SunStudyOverheadDistance (occluders, ground);
            out.overhead[s] = float (above);
            for (size_t l = 0; l < k; ++l) {
                const double level = options.levels[l];
                float& slot = out.free[s * k + l];
                if (above >= 0.0 && above <= level) {
                    slot = 0.0f; // the level is above a ceiling: no crown there
                    continue;
                }
                const double origin[3] = { x[s], y[s], groundZ[s] + level };
                double nearest = options.reach;
                for (const auto& dir : fan)
                    for (const EngineMasks& e : engines) {
                        if (e.engine == nullptr)
                            continue;
                        const uint32_t g = host[s];
                        const std::vector<uint8_t>& mask =
                            g < e.byHost.size () && !e.byHost[g].empty () ? e.byHost[g] : e.all;
                        const auto hit = e.engine->RaycastMasked (origin, dir.data (), nearest, mask);
                        if (hit.hit && hit.t < nearest)
                            nearest = hit.t;
                    }
                slot = float (nearest);
            }
            if (done)
                done->fetch_add (1);
        }
    };
    size_t threads = options.maxParallel;
    if (threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency ();
        threads = hw > 1 ? size_t (hw - 1) : 1;
    }
    threads = std::min (threads, std::max<size_t> (1, n / 64));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t)
        pool.emplace_back (work);
    work ();
    for (auto& t : pool)
        t.join ();
    if (stop.load ()) {
        out = CrownClearance ();
        error = "cancelled";
        return false;
    }
    return true;
}

} // namespace evp::sunstudy
