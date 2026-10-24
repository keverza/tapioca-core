#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <cmath>
#include <algorithm>

// OVERLAY-INVARIANTS.md: consume the existing view mapping only. No camera acquisition,
// SDK access, native geometry writes or extra document/history baseline lives here.
namespace geomsrv::archviz::edithud::detail {

namespace {

std::array<float, 12> Projection (const Map& m, edit::Vec3 origin)
{
    std::array<float, 12> out {};
    const edit::Vec3 offsets[] = { {}, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (size_t i = 0; i < 4; ++i) {
        ImVec2 p;
        if (m.Screen (origin + offsets[i], p)) {
            out[3 * i] = p.x;
            out[3 * i + 1] = p.y;
            out[3 * i + 2] = 1;
        }
    }
    return out;
}

bool Different (const std::array<float, 12>& a, const std::array<float, 12>& b)
{
    for (size_t i = 0; i < a.size (); ++i)
        if (std::abs (a[i] - b[i]) > 0.01f)
            return true;
    return false;
}

edit::Handle Held (const Session& s)
{
    return s.heldControl; // resolved where the gesture began: never re-read from the table
}

} // namespace

bool QuietPath (const Session& s, edit::Id id)
{
    return std::find (s.quietPaths.begin (), s.quietPaths.end (), id) != s.quietPaths.end ();
}

void UpdatePathDetail (Session& s, const Map& m, float scale)
{
    std::vector<edit::Id> quiet;
    for (const auto& path : Doc (s).paths) {
        edit::AlignmentGeometry g;
        std::string error;
        if (!edit::BuildAlignment (path.points, g, error))
            continue;
        double widest = 0;
        const auto sample = [&] (double station) {
            edit::StationFrame f;
            ImVec2 a, b;
            if (edit::Evaluate (path, g, station, f) && m.Screen (f.leftEdge, a) && m.Screen (f.rightEdge, b))
                widest = std::max (widest, double (std::hypot (a.x - b.x, a.y - b.y)) / std::max (scale, .01f));
        };
        for (size_t i = 0; i < path.sections.size (); ++i) {
            const double from = path.sections[i].station;
            const double to = i + 1 < path.sections.size () ? path.sections[i + 1].station : g.length;
            for (double t : { 0., .5, 1. })
                sample (from + (to - from) * t);
        }
        // Logical screen width, not a world-scale/camera heuristic. Hysteresis
        // avoids republishing at every tiny zoom change around the threshold.
        if (widest < (QuietPath (s, path.id) ? 32 : 24))
            quiet.push_back (path.id);
    }
    s.quietPaths = std::move (quiet);
    s.placed.erase (std::remove_if (s.placed.begin (), s.placed.end (),
                                    [&] (const Session::PlacedLabel& l) {
                                        return QuietPath (s, l.target.kerbPath != edit::kNoId
                                                                 ? l.target.kerbPath
                                                                 : l.target.value.ref.entity);
                                    }),
                    s.placed.end ());
}

bool DragOnto (const Session& s, const Map& m, ImVec2 mouse, double z, edit::Vec3 seed, edit::Vec3& out)
{
    if (!m.Onto (mouse.x, mouse.y, z, seed, out))
        return false;
    out = out + s.navigation.offset;
    return true;
}

void Navigate (Session& s, const Map& m, ImVec2 mouse)
{
    auto& n = s.navigation;
    if (!n.notice.empty () && s.message == n.notice)
        s.message.clear ();
    n.notice.clear ();
    n.blocked = false;
    if (ProfileDrag (s.drag))
        return; // native navigation does not change the panel canvas's independent mapping
    if (s.drag == Drag::None && !s.penDown) {
        n = {};
        n.blocked = m.v.navigating;
        return;
    }
    if (!n.projection) {
        n.reference = s.drag == Drag::Handle ? s.frame.origin : n.point.value_or (s.anchor);
        n.projection = Projection (m, n.reference);
        n.revision = m.v.navigationRevision;
    }
    const auto projection = Projection (m, n.reference);
    const bool changed = Different (*n.projection, projection) || n.revision != m.v.navigationRevision;
    n.projection = projection;
    n.revision = m.v.navigationRevision;
    if ((m.v.navigating || changed || n.pending) && ImGui::IsMouseReleased (0)) {
        if (s.drag != Drag::None)
            s.dragReleased = true;
        s.penDown = false;
    }
    if (m.v.navigating || changed) {
        n.pending = true;
        n.blocked = true;
    }
    else if (n.pending) {
        bool ok = true;
        if (s.drag == Drag::Handle) {
            const auto h = Held (s);
            ok = m.RayAt (mouse.x, mouse.y, s.frame.origin.z, s.frame.origin, n.ray);
            if (ok) {
                const auto neutral = edit::NeutralDrag (s.frame, h, n.ray);
                ok = neutral.ok;
                n.handle = { true, s.dragTransform, s.dragStarted ? s.dragValue : neutral.value, {} };
                n.handleRebased = ok;
            }
        }
        else if (n.point) {
            edit::Vec3 now;
            ok = m.Onto (mouse.x, mouse.y, n.point->z, *n.point, now);
            if (ok)
                n.offset = *n.point - now;
        }
        if (!ok) {
            n.blocked = true;
            n.notice =
                "Navigation paused: this view is edge-on to the edit. Navigate again, Enter finishes, Esc cancels.";
            s.message = n.notice;
            return;
        }
        n.pending = false;
        n.blocked = true; // the first resumed Move has zero delta, including repeated layouts
        s.dragPointerPause = { mouse.x, mouse.y };
        if (!s.dragStarted && s.drag != Drag::Marquee)
            s.startX = mouse.x, s.startY = mouse.y;
        if (s.penDown)
            s.penX = mouse.x, s.penY = mouse.y;
    }
    if (n.blocked && s.tracker.State () != edit::TrackerState::Editing) {
        n.notice = s.dragReleased ? "Edit paused: Enter finishes, Esc cancels. Navigation does not commit."
                   : n.pending    ? "Edit paused for navigation. Release navigation to resume; Esc cancels."
                                  : "Edit resumed: move the pointer to continue; Esc cancels.";
        s.message = n.notice;
    }
}

} // namespace geomsrv::archviz::edithud::detail
