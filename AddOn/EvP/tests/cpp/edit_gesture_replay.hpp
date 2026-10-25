#ifndef EVP_TESTS_EDIT_GESTURE_REPLAY_HPP
#define EVP_TESTS_EDIT_GESTURE_REPLAY_HPP

// Recorded editor gestures replayed against seeded scenes, for the kernel's share of an
// editing frame: what a locked editor asks the kernel for at every pointer move. It is the
// same kernel work, with or without the caches a frame may carry, so the determinism tests
// compare their results and the benchmark (benchmark_edit.cpp) times them.
//
// A frame here follows the host's calls: snap and pick at the pointer, then -- when the gesture
// edits -- the edit, the paved surface at the gesture's coarser step (edithud::PavedSurface)
// and the kerb corners the labels are placed from at the resting step (editlayers::PathLabels).
// No HUD layout, upload or Archicad redraw is in it (issue #47 measures those live).
#include "Edit/EditDocument.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
#include "Edit/EditSelection.hpp"
#include "Edit/EditSnap.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace editreplay {
namespace ed = geomsrv::edit;

struct Scene {
    std::string name;
    ed::Document doc;
    std::vector<ed::Id> paths;
};

// An `n` x `n` street grid, 20 m apart: every street crosses every other, so the network
// has n^2 junctions. Every third street bends half way with a 4 m radius, and a building
// outline sits in each block, so snapping has entities to consider as well as paths.
inline Scene Grid (const std::string& name, int n)
{
    Scene s;
    s.name = name;
    const double spacing = 20.0, length = (n - 1) * spacing + 10.0;
    for (int i = 0; i < n; ++i) {
        const double at = i * spacing;
        for (bool across : { false, true }) {
            std::vector<ed::Vec3> points;
            if (across)
                points = { { -5, at, 0 }, { length * 0.5, at, 0.2 }, { length - 5, at, 0.4 } };
            else
                points = { { at, -5, 0 }, { at, length * 0.5, 0.2 }, { at, length - 5, 0.4 } };
            if (i % 3 == 1) // a bend: the middle point moved off the line
                (across ? points[1].y : points[1].x) += 3.0;
            const ed::Id id = ed::AddPath (s.doc, points, 2.0, "street");
            if (i % 3 == 1) {
                std::string error;
                ed::SetPathRadius (s.doc, id, ed::FindPath (s.doc, id)->points[1].id, 4.0, error);
            }
            s.paths.push_back (id);
        }
    }
    for (int x = 0; x + 1 < n; ++x)
        for (int y = 0; y + 1 < n; ++y) {
            const double x0 = x * spacing + 4, y0 = y * spacing + 4;
            ed::AddPolyline (s.doc, { { x0, y0, 0 }, { x0 + 8, y0, 0 }, { x0 + 8, y0 + 8, 0 }, { x0, y0 + 8, 0 } },
                             true);
        }
    return s;
}

enum class Gesture { Hover, Drag, Width };
inline const char* GestureName (Gesture g)
{
    return g == Gesture::Hover ? "hover" : g == Gesture::Drag ? "drag" : "width";
}

// What a frame may carry from one frame to the next. Null members are not used.
struct Caches {
    ed::PickGeometryCache* pick = nullptr;
    ed::PathOutlineCache* outlines = nullptr; // shared by the snap and both pavings; pruned per frame
};

// Everything a frame produced that a cache must not change.
struct FrameResult {
    bool snapped = false;
    ed::Vec3 snap;
    ed::SnapKind kind = ed::SnapKind::None;
    std::vector<ed::Ref> hits;
    size_t rings = 0, ringPoints = 0, corners = 0, triangles = 0;
    bool edited = false;
    bool operator== (const FrameResult& o) const
    {
        return snapped == o.snapped && kind == o.kind && std::abs (snap.x - o.snap.x) < 1e-12 &&
               std::abs (snap.y - o.snap.y) < 1e-12 && std::abs (snap.z - o.snap.z) < 1e-12 && hits == o.hits &&
               rings == o.rings && ringPoints == o.ringPoints && corners == o.corners && triangles == o.triangles &&
               edited == o.edited;
    }
};

struct Options {
    double pixelsPerMetre = 40.0; // a plan at about 1:100 on screen
    double pickRadius = 8.0;      // pixels
    double pathStep = 0.5;        // the layers' sampling step at rest
    // The surface's step while a gesture edits, as edithud::SurfaceStep chooses it: the preview's
    // triangle budget, never finer than `gestureFinest`. A positive `gestureStep` fixes it instead.
    size_t gestureTriangles = 4000;
    double gestureFinest = 2.0;
    double gestureStep = 0.0;
};

// Where a frame's time went, in milliseconds.
struct Parts {
    double edit = 0, snap = 0, pick = 0, corners = 0, surface = 0;
};

inline ed::PickView PlanView (const Options& o)
{
    ed::PickView view;
    view.toScreen = [k = o.pixelsPerMetre] (ed::Vec3 p, double& x, double& y) {
        x = p.x * k;
        y = -p.y * k;
        return true;
    };
    return view;
}

// The pointer at frame `i` of `frames`: a sweep across the scene's first junction, so hover
// and drag run past edges, corners and a building.
inline ed::Vec3 Pointer (int i, int frames)
{
    const double t = frames > 1 ? double (i) / (frames - 1) : 0.0;
    return { -3.0 + 16.0 * t, -2.0 + 9.0 * t, 0.0 };
}

// One frame: the edit the gesture makes (a moved control point or a width), then snap, pick
// and the paved network. `doc` is changed by Drag and Width.
inline FrameResult RunFrame (ed::Document& doc, const Scene& scene, Gesture gesture, int i, int frames,
                             const Options& o, const Caches& caches, Parts* parts = nullptr)
{
    using Clock = std::chrono::steady_clock;
    Parts local;
    Parts& spent = parts != nullptr ? *parts : local;
    auto mark = Clock::now ();
    const auto lap = [&] (double& into) {
        const auto now = Clock::now ();
        into += std::chrono::duration<double, std::milli> (now - mark).count ();
        mark = now;
    };
    FrameResult r;
    const ed::Vec3 cursor = Pointer (i, frames);
    std::string error;
    const ed::Id moved = scene.paths.front ();
    const ed::Path* path = ed::FindPath (doc, moved);
    if (gesture == Gesture::Drag && path != nullptr)
        r.edited = ed::MovePathPoint (doc, moved, path->points.front ().id, cursor, error);
    else if (gesture == Gesture::Width && path != nullptr) {
        const double w = 0.5 + 2.0 * i / std::max (1, frames - 1);
        r.edited = ed::SetPathWidth (doc, moved, path->sections.front ().id, w, w, error);
    }
    lap (spent.edit);
    ed::SnapQuery q;
    q.cursor = cursor;
    q.tolerance = o.pickRadius / o.pixelsPerMetre;
    q.pathStep = o.pathStep;
    q.outlines = caches.outlines;
    if (gesture == Gesture::Drag && path != nullptr)
        q.exclude.push_back ({ moved, ed::Part::PathPoint, path->points.front ().id });
    const ed::SnapResult snap = ed::Snap (doc, {}, q);
    r.snapped = snap.snapped;
    r.snap = snap.point;
    r.kind = snap.kind;
    lap (spent.snap);
    if (gesture == Gesture::Hover) {
        double x = 0, y = 0;
        const ed::PickView view = PlanView (o);
        view.toScreen (cursor, x, y);
        for (const auto& h : ed::Pick (doc, view, x, y, ed::kPickAll, o.pickRadius, caches.pick))
            r.hits.push_back (h.ref);
        lap (spent.pick);
        return r;
    }
    // An edit redraws the network: the drawn surface and the labelled kerb corners.
    const double step =
        o.gestureStep > 0 ? o.gestureStep : ed::PreviewStep (doc.paths, o.gestureTriangles, o.gestureFinest);
    const ed::PavedArea surface = ed::Pave (doc.paths, step, true, caches.outlines);
    lap (spent.surface);
    const ed::PavedArea corners = ed::Pave (doc.paths, o.pathStep, false, caches.outlines);
    lap (spent.corners);
    r.corners = corners.corners.size ();
    r.rings = surface.rings.size ();
    for (const auto& ring : surface.rings)
        r.ringPoints += ring.size ();
    r.triangles = surface.triangles.size () / 3;
    return r;
}

struct Timing {
    std::vector<double> ms; // one per frame
    Parts parts;            // summed over the frames
    double Percentile (double p) const
    {
        if (ms.empty ())
            return 0.0;
        std::vector<double> sorted = ms;
        std::sort (sorted.begin (), sorted.end ());
        const size_t at =
            std::min (sorted.size () - 1, size_t (std::max (1.0, std::ceil (p * sorted.size () - 1e-9))) - 1);
        return sorted[at];
    }
};

// The gesture over `frames` frames on a copy of the scene's document; each frame's result
// goes to `results` when it is given.
inline Timing Replay (const Scene& scene, Gesture gesture, int frames, const Options& o, const Caches& caches,
                      std::vector<FrameResult>* results = nullptr)
{
    Timing t;
    ed::Document doc = scene.doc;
    for (int i = 0; i < frames; ++i) {
        const auto start = std::chrono::steady_clock::now ();
        FrameResult r = RunFrame (doc, scene, gesture, i, frames, o, caches, &t.parts);
        t.ms.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ());
        if (caches.outlines != nullptr)
            caches.outlines->Prune ();
        if (results != nullptr)
            results->push_back (std::move (r));
    }
    return t;
}

} // namespace editreplay
#endif
