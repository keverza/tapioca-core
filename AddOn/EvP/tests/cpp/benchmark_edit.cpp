// Opt-in measurement executable, never linked into the add-on or the tests: the kernel's
// share of a locked editor's frame, replayed (edit_gesture_replay.hpp). One line per scene and
// gesture, uncached and with the frame caches, so a change to either shows as a number.
//
//   TapiocaEditBench [frames]        default 120 frames per gesture
#include "edit_gesture_replay.hpp"
#include <cstdio>
#include <cstdlib>

namespace er = editreplay;

int main (int argc, char** argv)
{
    const int frames = argc > 1 ? std::max (2, std::atoi (argv[1])) : 120;
    const er::Options options;
    std::printf ("scene,paths,gesture,caches,frames,p50_ms,p95_ms,max_ms,edit,snap,pick,surface,corners\n");
    for (const auto& [name, n] : { std::pair { "small", 2 }, std::pair { "medium", 5 }, std::pair { "stress", 10 } }) {
        const er::Scene scene = er::Grid (name, n);
        for (auto g : { er::Gesture::Hover, er::Gesture::Drag, er::Gesture::Width })
            for (bool cached : { false, true }) {
                geomsrv::edit::PickGeometryCache pick;
                er::Caches caches;
                if (cached)
                    caches.pick = &pick;
                er::Replay (scene, g, 3, options, caches); // warm the allocator and the caches' first build
                const er::Timing t = er::Replay (scene, g, frames, options, caches);
                const auto mean = [&] (double total) { return total / frames; };
                std::printf ("%s,%zu,%s,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", name, scene.paths.size (),
                             er::GestureName (g), cached ? "on" : "off", frames, t.Percentile (0.5),
                             t.Percentile (0.95), t.Percentile (1.0), mean (t.parts.edit), mean (t.parts.snap),
                             mean (t.parts.pick), mean (t.parts.surface), mean (t.parts.corners));
            }
    }
    return 0;
}
