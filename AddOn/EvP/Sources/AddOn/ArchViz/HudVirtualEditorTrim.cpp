#include "ArchViz/HudVirtualEditorDetail.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud::detail {

namespace {

bool SameCandidate (const TrimPreview& preview, const edit::Document& doc, const edit::Ref& target, edit::Vec3 pointer,
                    bool mapped)
{
    return preview.revision == doc.revision && preview.span.entity == target.entity &&
           preview.span.segment == target.item && preview.pointer == pointer && preview.mapped == mapped;
}

double ScreenStation (ImVec2 point, ImVec2 start, double dx, double dy, double length2)
{
    return ((point.x - start.x) * dx + (point.y - start.y) * dy) / length2;
}

// A 3D segment still projects to a screen line, but perspective does not interpolate its model
// parameter linearly in pixels. Recover that parameter on the picked segment itself: a horizontal
// plane at its midpoint's height chooses the wrong side of a crossing for a sloped line.
bool PointerOnSegment (const Map& m, ImVec2 mouse, const edit::Entity& entity, edit::Id segment, edit::Vec3& pointer)
{
    const int index = edit::SegmentIndex (entity, segment);
    if (index < 0)
        return false;
    size_t from = 0, to = 0;
    edit::SegmentEnds (entity, size_t (index), from, to);
    const edit::Vec3 a = entity.vertices[from].p, b = entity.vertices[to].p;
    ImVec2 pa, pb;
    if (!m.Screen (a, pa) || !m.Screen (b, pb))
        return false;
    const double dx = pb.x - pa.x, dy = pb.y - pa.y;
    const double length2 = dx * dx + dy * dy;
    if (!(length2 > 1.0e-12) || !std::isfinite (length2))
        return false;
    const double target = std::clamp (ScreenStation (mouse, pa, dx, dy, length2), 0.0, 1.0);
    if (target == 0.0 || target == 1.0) {
        pointer = target == 0.0 ? a : b;
        return true;
    }

    double low = 0.0, high = 1.0;
    for (int iteration = 0; iteration < 32; ++iteration) {
        const double t = (low + high) * 0.5;
        ImVec2 projected;
        if (!m.Screen (edit::Lerp (a, b, t), projected))
            return false;
        if (ScreenStation (projected, pa, dx, dy, length2) < target)
            low = t;
        else
            high = t;
    }
    pointer = edit::Lerp (a, b, (low + high) * 0.5);
    return edit::Finite (pointer);
}

} // namespace

void UpdateTrimPreview (Session& s, const Map& m, ImVec2 mouse, bool hovered, double radius)
{
    if (s.tool != Tool::Trim || !hovered || s.drag != Drag::None) {
        s.trimPreview.reset ();
        return;
    }
    const auto hits = edit::Pick (Doc (s), m.Pick (), mouse.x, mouse.y, edit::kPickSegment, radius, &s.pickCache);
    const edit::PickHit* picked = nullptr;
    const edit::Entity* entity = nullptr;
    for (const auto& hit : hits)
        if (hit.ref.part == edit::Part::Segment && (entity = edit::Find (Doc (s), hit.ref.entity)) != nullptr) {
            picked = &hit;
            break;
        }
    if (picked == nullptr) {
        s.trimPreview.reset ();
        return;
    }

    edit::Vec3 pointer;
    const bool mapped = PointerOnSegment (m, mouse, *entity, picked->ref.item, pointer);
    if (s.trimPreview && SameCandidate (*s.trimPreview, Doc (s), picked->ref, pointer, mapped))
        return;

    TrimPreview next;
    next.revision = Doc (s).revision;
    next.pointer = pointer;
    next.span.entity = picked->ref.entity;
    next.span.segment = picked->ref.item;
    next.mapped = mapped;
    if (!mapped)
        next.error = "This view cannot resolve the trim pointer on the picked segment.";
    else
        next.valid = edit::FindTrimSpan (Doc (s), picked->ref.entity, picked->ref.item, pointer, next.span, next.error);
    if (!next.valid)
        next.span.removed.clear ();
    s.trimPreview = std::move (next);
}

void DrawTrimPreview (Session& s, const Map& m, float scale)
{
    if (s.tool != Tool::Trim || s.drag != Drag::None || !s.trimPreview || !s.trimPreview->valid ||
        s.trimPreview->revision != Doc (s).revision)
        return;
    const auto& removed = s.trimPreview->span.removed;
    for (size_t i = 0; i + 1 < removed.size (); ++i)
        Line (s, m, removed[i], removed[i + 1], kAccent, 5.0f * scale);
    if (!removed.empty ()) {
        Dot (s, m, removed.front (), 5.0f * scale, kAccent);
        Dot (s, m, removed.back (), 5.0f * scale, kAccent);
    }
}

void ClickTrim (Session& s)
{
    if (!s.trimPreview) {
        s.message = "Hover a polyline segment with a removable span, then click.";
        return;
    }
    if (s.trimPreview->revision != Doc (s).revision) {
        s.trimPreview.reset ();
        s.message = "The document changed; hover the trim span again.";
        return;
    }
    if (!s.trimPreview->valid) {
        s.message = s.trimPreview->error;
        return;
    }

    const edit::Id entity = s.trimPreview->span.entity;
    const edit::Id segment = s.trimPreview->span.segment;
    const edit::Vec3 pointer = s.trimPreview->pointer;
    if (!s.edits.Apply ([entity, segment, pointer] (edit::Document& doc, std::string& error) {
            return edit::TrimBetweenCrossings (doc, entity, segment, pointer, error);
        })) {
        s.message = s.edits.Message ();
        s.trimPreview.reset ();
        return;
    }
    s.selection.Prune (Doc (s));
    s.selection.hover = {};
    s.trimPreview.reset ();
    s.message.clear ();
}

} // namespace geomsrv::archviz::edithud::detail
