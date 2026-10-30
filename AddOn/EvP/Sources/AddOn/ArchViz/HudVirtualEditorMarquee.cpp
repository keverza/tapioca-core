#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditOrganize.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud::detail {

namespace {
// The pointer's travel from the press that still makes a click, and the lasso's sampling step.
constexpr float kClickPixels = 3.0f;
} // namespace

void BeginMarquee (Session& s, ImVec2 mouse, bool additive, bool ctrl, std::vector<edit::PickHit> pressed)
{
    s.drag = Drag::Marquee;
    s.marqueeAdditive = additive;
    s.marqueeLasso = s.lasso != ctrl;
    s.lassoPoints = { { mouse.x, mouse.y } };
    s.pressHits = std::move (pressed);
    s.startX = mouse.x, s.startY = mouse.y;
}

// A press that never left its point is a click on what it was pressed inside (or nothing);
// otherwise the lasso's outline or the rectangle selects.
bool SelectGroups (Session& s, const std::vector<edit::Ref>& before)
{
    const auto& doc = Doc (s);
    if (edit::FindGroup (doc, s.enteredGroup) == nullptr)
        s.enteredGroup = edit::kNoId;
    std::vector<edit::Ref> items = s.selection.Items ();
    for (const auto& r : before)
        if (r.part == edit::Part::Entity && !s.selection.Contains (r)) {
            const auto group = edit::WidenToGroups (doc, { r }, s.enteredGroup);
            std::erase_if (items, [&] (const edit::Ref& x) {
                return std::find (group.begin (), group.end (), x) != group.end ();
            });
        }
    const auto widened = edit::WidenToGroups (doc, items, s.enteredGroup);
    const bool grew = widened.size () > items.size ();
    if (widened != s.selection.Items ())
        s.selection.Set (widened);
    return grew;
}

void GroupSelection (Session& s, bool group)
{
    if (s.drag != Drag::None || !s.drawing.empty ())
        return;
    const auto items = s.selection.Items ();
    const edit::Id within = s.enteredGroup;
    const bool ok = s.edits.Apply ([items, group, within] (edit::Document& d, std::string& error) {
        return group ? edit::GroupObjects (d, items, {}, error, within) != edit::kNoId
                     : edit::Ungroup (d, items, error, within);
    });
    if (!ok) {
        s.message = s.edits.Message ();
        return;
    }
    s.message = group ? "Grouped: a click now picks them together. Double-click to edit inside the group."
                      : "Ungrouped one level.";
}

bool EnterGroup (Session& s, const edit::Ref& ref)
{
    const auto& doc = Doc (s);
    const edit::Membership* m = edit::MemberOf (doc, ref.entity);
    if (m == nullptr || m->group == edit::kNoId)
        return false;
    // One level further in: the group just inside the entered one on the object's way out.
    edit::Id next = edit::kNoId;
    for (const edit::Group* g = edit::FindGroup (doc, m->group); g != nullptr; g = edit::FindGroup (doc, g->parent)) {
        if (g->id == s.enteredGroup)
            break;
        next = g->id;
    }
    if (next == edit::kNoId)
        return false; // already in its own group
    s.enteredGroup = next;
    s.selection.Set ({ { ref.entity, edit::Part::Entity, edit::kNoId } });
    SelectGroups (s, {});
    s.message = "Editing inside " + edit::FindGroup (doc, next)->name + ". Esc with nothing selected leaves it.";
    return true;
}

void CommitMarquee (Session& s, const edit::PickView& pick, float x, float y)
{
    const std::vector<edit::Ref> before = s.selection.Items ();
    if (std::hypot (x - s.startX, y - s.startY) <= kClickPixels) {
        if (!s.pressHits.empty ())
            s.selection.Click (s.pressHits, s.startX, s.startY, s.marqueeAdditive);
    }
    else if (s.marqueeLasso) {
        s.lassoPoints.push_back ({ x, y });
        s.selection.Lasso (Doc (s), pick, s.lassoPoints, s.filter, s.marqueeAdditive);
    }
    else
        s.selection.Marquee (Doc (s), pick, s.startX, s.startY, x, y, s.filter, s.marqueeAdditive);
    SelectGroups (s, before);
    s.pressHits.clear ();
    s.lassoPoints.clear ();
    s.marqueeLasso = false;
    s.drag = Drag::None;
}

void DrawMarquee (Session& s, ImDrawList* draw, ImVec2 mouse)
{
    if (s.drag != Drag::Marquee || !(ImGui::IsMouseDown (0) || s.dragReleased))
        return;
    if (s.marqueeLasso) {
        // The lasso follows the pointer while held, a point every few pixels; drawn closed back
        // to its start, filled as the contained marquee is.
        if (ImGui::IsMouseDown (0) && !s.navigation.blocked &&
            (s.lassoPoints.empty () || std::hypot (mouse.x - s.lassoPoints.back ().first,
                                                   mouse.y - s.lassoPoints.back ().second) >= kClickPixels))
            s.lassoPoints.push_back ({ mouse.x, mouse.y });
        if (s.lassoPoints.size () < 2)
            return;
        std::vector<ImVec2> outline;
        for (const auto& [x, y] : s.lassoPoints)
            outline.push_back ({ float (x), float (y) });
        if (outline.size () >= 3)
            draw->AddConcavePolyFilled (outline.data (), int (outline.size ()), IM_COL32 (62, 99, 221, 40));
        draw->AddPolyline (outline.data (), int (outline.size ()), kAxis[2], ImDrawFlags_Closed, 1.5f);
        return;
    }
    // The rectangle, in screen pixels: left to right solid (contained), right to left thin (touched).
    const bool crossing = mouse.x < s.startX;
    const ImVec2 lo { std::min (s.startX, mouse.x), std::min (s.startY, mouse.y) };
    const ImVec2 hi { std::max (s.startX, mouse.x), std::max (s.startY, mouse.y) };
    draw->AddRectFilled (lo, hi, crossing ? IM_COL32 (70, 167, 88, 40) : IM_COL32 (62, 99, 221, 40));
    draw->AddRect (lo, hi, crossing ? kAxis[1] : kAxis[2], 0.0f, 0, crossing ? 1.0f : 1.5f);
}

} // namespace geomsrv::archviz::edithud::detail
