#include "Edit/EditGradeToControls.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditPath.hpp"
#include "Edit/EditPathEdit.hpp"
#include <algorithm>
#include <cmath>

namespace tapioca::geometry {
namespace edit = geomsrv::edit;

namespace {
constexpr const char* kBinding = "control"; // what a break fixed by this command records

// Every enabled control's points: the vertex it names, or each vertex of its whole polyline.
std::vector<edit::Vec3> ControlPoints (const edit::Document& doc)
{
    std::vector<edit::Vec3> out;
    if (!doc.landscape)
        return out;
    for (const auto& control : doc.landscape->terrainControls) {
        if (!control.enabled)
            continue;
        const auto source = std::find_if (doc.landscape->sources.begin (), doc.landscape->sources.end (),
                                          [&] (const LandscapeSource& s) { return s.id == control.source; });
        if (source == doc.landscape->sources.end () || source->deleted)
            continue;
        const edit::Entity* e = edit::Find (doc, source->entityId);
        if (e == nullptr)
            continue;
        for (const auto& v : e->vertices)
            if (control.vertex == edit::kNoId || v.id == control.vertex)
                out.push_back (v.p);
    }
    return out;
}

struct Hit {
    double station = 0.0, distance = 0.0, z = 0.0;
};
} // namespace

std::string GradeToControlsReport::Summary () const
{
    if (controls == 0)
        return "No control points: set guide vertices as fixed heights first.";
    if (graded.empty ())
        return "No control point lies within reach of the selected paths; nothing changed.";
    size_t points = 0;
    for (const auto& g : graded)
        points += g.points;
    std::string text = "Graded " + std::to_string (graded.size ()) + " path" + (graded.size () == 1 ? "" : "s") +
                       " to " + std::to_string (points) + " control point" + (points == 1 ? "" : "s") + ".";
    if (!untouched.empty ())
        text += " " + std::to_string (untouched.size ()) + " had none in reach and kept their heights.";
    if (skipped > 0)
        text += " " + std::to_string (skipped) + " control point" + (skipped == 1 ? " was" : "s were") +
                " left out: another lay nearer at the same place, or a different height is already fixed there.";
    return text;
}

bool GradePathsToControls (edit::Document& doc, const std::vector<edit::Id>& paths, double radius,
                           GradeToControlsReport& report, std::string& error)
{
    report = {};
    if (!(radius > 0.0) || !std::isfinite (radius)) {
        error = "the reach must be a positive distance";
        return false;
    }
    if (paths.empty ()) {
        error = "select the paths to grade";
        return false;
    }
    const auto controls = ControlPoints (doc);
    report.controls = controls.size ();
    for (const edit::Id id : paths) {
        const edit::Path* path = edit::FindPath (doc, id);
        if (path == nullptr) {
            error = "a selected path no longer exists";
            return false;
        }
        edit::AlignmentGeometry g;
        if (!edit::BuildAlignment (path->points, g, error))
            return false;
        // Each control in reach, at its station; the one nearer the centreline wins a place.
        std::vector<Hit> hits;
        for (const auto& p : controls) {
            double station = 0.0, offset = 0.0;
            edit::Placement at;
            if (!edit::StationOf (g, p, station, offset) || !edit::Locate (g, station, at))
                continue;
            const double distance = std::hypot (p.x - at.point.x, p.y - at.point.y);
            if (distance <= radius)
                hits.push_back ({ station, distance, p.z });
        }
        std::sort (hits.begin (), hits.end (), [] (const Hit& a, const Hit& b) { return a.distance < b.distance; });
        std::vector<Hit> kept;
        for (const auto& h : hits) {
            if (std::any_of (kept.begin (), kept.end (),
                             [&] (const Hit& k) { return std::abs (k.station - h.station) < radius; })) {
                ++report.skipped;
                continue;
            }
            kept.push_back (h);
        }
        if (kept.empty ()) {
            report.untouched.push_back (id);
            continue;
        }
        std::sort (kept.begin (), kept.end (), [] (const Hit& a, const Hit& b) { return a.station < b.station; });
        std::vector<edit::Id> ids;
        for (size_t i = 0; i < kept.size (); ++i)
            ids.push_back (edit::NewId (doc));
        size_t applied = 0, refused = 0;
        if (!edit::EditPath (
                doc, id,
                [&] (edit::Path& p, std::string& why) {
                    std::vector<edit::Id> used;
                    for (size_t k = 0; k < kept.size (); ++k) {
                        const Hit& h = kept[k];
                        // The nearest break within reach not already taken by another control.
                        int best = -1;
                        for (size_t i = 0; i < p.profile.size (); ++i) {
                            const auto& b = p.profile[i];
                            if (std::abs (b.station - h.station) > radius ||
                                std::find (used.begin (), used.end (), b.id) != used.end ())
                                continue;
                            if (best < 0 || std::abs (b.station - h.station) <
                                                std::abs (p.profile[size_t (best)].station - h.station))
                                best = int (i);
                        }
                        edit::Id target = edit::kNoId;
                        if (best >= 0) {
                            const auto& b = p.profile[size_t (best)];
                            if (b.fixed && b.binding != kBinding && b.z != h.z) {
                                ++refused; // another fixed height (a door, a kerb) is kept
                                continue;
                            }
                            target = b.id;
                            const bool end = best == 0 || size_t (best) + 1 == p.profile.size ();
                            if (!end && std::abs (b.station - h.station) > 1.0e-9 &&
                                !edit::MoveBreak (p.profile, target, h.station, why))
                                return false;
                        }
                        else {
                            if (!edit::InsertBreak (p.profile, h.station, ids[k], why))
                                return false;
                            target = ids[k];
                        }
                        for (auto& b : p.profile)
                            if (b.id == target) {
                                b.z = h.z;
                                b.fixed = true;
                                b.binding = kBinding;
                            }
                        used.push_back (target);
                        ++applied;
                    }
                    return applied > 0;
                },
                error)) {
            if (applied == 0 && error.empty ()) { // every control there was refused: nothing changed
                report.skipped += refused;
                report.untouched.push_back (id);
                continue;
            }
            return false;
        }
        report.skipped += refused;
        report.graded.push_back ({ id, applied });
    }
    return true;
}

} // namespace tapioca::geometry
