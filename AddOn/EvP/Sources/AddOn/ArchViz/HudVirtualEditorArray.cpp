#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudLengthField.hpp"
#include "ArchViz/HudShell.hpp"
#include "Edit/EditArrange.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud::detail {
namespace {

constexpr size_t kMaxPreviewLines = 20000;
constexpr double kPreviewMaxAngle = 0.2;
constexpr ImU32 kArrayHandle = IM_COL32 (225, 40, 225, 255); // #130: the array's own handles

bool PathOutline (const edit::Path& path, RectangularArrayDraft::Outline& outline, std::string& error)
{
    edit::AlignmentGeometry alignment;
    if (!edit::BuildAlignment (path.points, alignment, error))
        return false;
    bool first = true;
    for (const auto& element : alignment.elements) {
        const int pieces = element.kind == edit::ElementKind::Arc
                               ? std::max (1, int (std::ceil (std::abs (element.sweep) / kPreviewMaxAngle)))
                               : 1;
        std::vector<double> stations;
        for (int i = 0; i <= pieces; ++i)
            stations.push_back (element.start + element.length * i / pieces);
        for (const auto& grade : path.profile) {
            if (grade.station >= element.start && grade.station <= element.start + element.length)
                stations.push_back (grade.station); // a straight alignment can still carry a grade break
            const double curveStart = std::max (element.start, grade.station - grade.curve * 0.5);
            const double curveEnd = std::min (element.start + element.length, grade.station + grade.curve * 0.5);
            for (int i = 0; i <= 8 && curveStart < curveEnd; ++i)
                stations.push_back (curveStart + (curveEnd - curveStart) * i / 8);
        }
        std::sort (stations.begin (), stations.end ());
        stations.erase (std::unique (stations.begin (), stations.end (),
                                     [] (double a, double b) { return std::abs (a - b) <= 1.0e-9; }),
                        stations.end ());
        for (size_t i = first ? 0 : 1; i < stations.size (); ++i) {
            edit::Placement at;
            double z = 0.0;
            if (!edit::Locate (alignment, stations[i], at) || !edit::Elevation (path.profile, stations[i], z)) {
                error = "the selected path's alignment or profile cannot be previewed";
                return false;
            }
            outline.points.push_back ({ at.point.x, at.point.y, z });
        }
        first = false;
    }
    outline.closed = false;
    return outline.points.size () >= 2;
}

bool SameNumber (double a, double b)
{
    return a == b || (std::isnan (a) && std::isnan (b));
}

bool Cached (const RectangularArrayDraft& draft)
{
    if (draft.builtPolar != draft.polar)
        return false;
    if (draft.polar)
        return draft.builtCount == draft.count && SameNumber (draft.builtSweep, draft.sweepDegrees) &&
               draft.builtRotate == draft.rotate && SameNumber (draft.builtCentre.x, draft.centre.x) &&
               SameNumber (draft.builtCentre.y, draft.centre.y);
    return draft.builtX == draft.countX && draft.builtY == draft.countY &&
           SameNumber (draft.builtSpacingX, draft.spacingX) && SameNumber (draft.builtSpacingY, draft.spacingY);
}

constexpr double kDegrees = 3.14159265358979323846 / 180.0;

// #130: the kernel's own refusals, said before Apply.
bool PolarPreviewValid (const RectangularArrayDraft& draft, std::string& error)
{
    if (draft.count <= 0 || draft.count > 10000) {
        error = "Polar count must be positive, at most 10000, and include the source.";
        return false;
    }
    if (draft.outlines.size () > 100000 / size_t (draft.count)) {
        error = "Polar array exceeds the 100000 object-placement budget.";
        return false;
    }
    if (!std::isfinite (draft.sweepDegrees) || std::abs (draft.sweepDegrees) > 360.0 + 1.0e-7 ||
        (draft.count > 1 && draft.sweepDegrees == 0.0)) {
        error = "The angle must be nonzero and at most a full turn (360 degrees).";
        return false;
    }
    if (!edit::Finite (draft.centre)) {
        error = "The centre is not a finite point.";
        return false;
    }
    return true;
}

bool PreviewCells (const RectangularArrayDraft& draft, size_t& cells, std::string& error)
{
    if (draft.countX <= 0 || draft.countY <= 0) {
        error = "Array counts must be positive and include the source.";
        return false;
    }
    const size_t x = size_t (draft.countX), y = size_t (draft.countY);
    if (x > 10000 || y > 10000 / x) {
        error = "Rectangular array counts must contain at most 10000 cells.";
        return false;
    }
    cells = x * y;
    if (draft.outlines.size () > 100000 / cells) {
        error = "Rectangular array exceeds the 100000 object-placement budget.";
        return false;
    }
    if (!std::isfinite (draft.spacingX) || !std::isfinite (draft.spacingY) || (x > 1 && draft.spacingX == 0.0) ||
        (y > 1 && draft.spacingY == 0.0)) {
        error = "Spacing must be finite and nonzero on each repeated axis.";
        return false;
    }
    const edit::Vec3 last { double (x - 1) * draft.spacingX, double (y - 1) * draft.spacingY, 0.0 };
    if (!edit::Finite (last)) {
        error = "Rectangular array placement exceeds finite coordinates.";
        return false;
    }
    for (const auto& outline : draft.outlines)
        for (const auto& point : outline.points)
            if (!edit::Finite (point) || !edit::Finite (point + last)) {
                error = "Rectangular array placement exceeds finite coordinates.";
                return false;
            }
    return true;
}

} // namespace

namespace {
// The selection's outlines and the centre of its extent, read from the document as it is now.
bool Capture (const edit::Document& doc, const std::vector<edit::Ref>& refs, RectangularArrayDraft& draft,
              std::string& error)
{
    const edit::Clipboard clip = edit::Copy (doc, refs);
    if (clip.Empty ()) {
        error = "Select the objects to array.";
        return false;
    }
    std::vector<RectangularArrayDraft::Outline> outlines;
    for (const auto& path : clip.paths) {
        RectangularArrayDraft::Outline outline;
        if (!PathOutline (path, outline, error)) {
            error = "Rectangular array preview refused: " + error;
            return false;
        }
        outlines.push_back (std::move (outline));
    }
    for (const auto& entity : clip.entities)
        for (auto& ring : edit::ShapeRings (entity, kPreviewMaxAngle)) { // a compound: each contour
            RectangularArrayDraft::Outline outline;
            outline.points = std::move (ring);
            outline.closed = entity.closed;
            if (outline.points.size () < 2) {
                error = "Rectangular array preview needs an outline for every selected object.";
                return false;
            }
            outlines.push_back (std::move (outline));
        }
    draft.outlines = std::move (outlines);
    edit::Centre (clip, draft.middle);
    return true;
}
} // namespace

bool CheckRectangularArrayDraft (Session& s)
{
    if (!s.rectangularArray)
        return false;
    auto& draft = *s.rectangularArray;
    // The user, 2026-11-20: "do not cancel the array command if the object was moved -- it should
    // allow repositioning the parent geometry during the command". The same selection, tool and mode
    // keep the draft; a document that changed under it is read again, so the preview follows the
    // moved source. Only a selection, tool or mode change -- or a source that is gone -- closes it.
    const bool same =
        draft.refs == s.selection.Items () && draft.tool == s.tool && draft.landscapeMode == s.landscapeMode;
    if (same && draft.revision == Doc (s).revision)
        return true;
    std::string error;
    if (same && Capture (Doc (s), draft.refs, draft, error)) {
        draft.revision = Doc (s).revision;
        draft.builtPolar = !draft.polar; // the cache is of the old outlines: rebuilt at the next update
        return true;
    }
    s.rectangularArray.reset ();
    s.message = same ? "Array draft closed: " + error : "Array draft closed: its selection or tool changed.";
    return false;
}

void CancelRectangularArray (Session& s)
{
    s.rectangularArray.reset ();
    s.message.clear ();
}

namespace {
bool BeginArray (Session& s, bool polar)
{
    if (s.rectangularArray && CheckRectangularArrayDraft (s)) {
        if (s.rectangularArray->polar != polar) { // the other kind asked for: the same draft, switched
            s.rectangularArray->polar = polar;
            UpdateRectangularArrayPreview (s);
        }
        return true;
    }
    if (s.tool != Tool::Select || s.landscapeMode != LandscapeMode::Select || s.edits.InGesture () ||
        GizmoModeLocked (s) || !s.drawing.empty () || s.terrainCapture || s.draftingImport) {
        s.message = polar ? "Finish the current input or Esc it, then start the polar array from Select."
                          : "Finish the current input or Esc it, then start the rectangular array from Select.";
        return false;
    }
    RectangularArrayDraft draft;
    draft.refs = s.selection.Items ();
    draft.revision = Doc (s).revision;
    draft.tool = s.tool;
    draft.landscapeMode = s.landscapeMode;
    draft.polar = polar;
    draft.centre = GizmoFrame (s).origin;
    if (std::string error; !Capture (Doc (s), draft.refs, draft, error)) {
        s.message = error;
        return false;
    }
    s.rectangularArray = std::move (draft);
    s.message.clear ();
    UpdateRectangularArrayPreview (s);
    return true;
}
} // namespace

bool BeginRectangularArray (Session& s)
{
    return BeginArray (s, false);
}

bool BeginPolarArray (Session& s)
{
    return BeginArray (s, true);
}

namespace {
// #130: every copy's outline at its angle, by the kernel's own step and turn, so the preview is
// what Apply makes.
void PolarPreview (RectangularArrayDraft& draft)
{
    if (!PolarPreviewValid (draft, draft.previewError))
        return;
    const size_t count = size_t (draft.count);
    const double step = edit::PolarStep (count, draft.sweepDegrees * kDegrees);
    for (size_t k = 1; k < count; ++k) {
        const double angle = step * double (k);
        const edit::Vec3 offset = edit::TurnInPlan (draft.middle, draft.centre, angle) - draft.middle;
        const auto place = [&] (edit::Vec3 p) {
            return draft.rotate ? edit::TurnInPlan (p, draft.centre, angle) : p + offset;
        };
        for (const auto& outline : draft.outlines) {
            const size_t segments = outline.points.size () - (outline.closed ? 0 : 1);
            for (size_t i = 0; i < segments; ++i) {
                if (draft.lines.size () == kMaxPreviewLines) {
                    draft.truncated = true;
                    return;
                }
                draft.lines.push_back (
                    { place (outline.points[i]), place (outline.points[(i + 1) % outline.points.size ()]) });
            }
        }
    }
}
} // namespace

void UpdateRectangularArrayPreview (Session& s)
{
    if (!CheckRectangularArrayDraft (s))
        return;
    auto& draft = *s.rectangularArray;
    if (draft.polar && draft.centreAtPivot)
        draft.centre = GizmoFrame (s).origin; // a repositioned gumball moves the axis
    if (Cached (draft))
        return;
    draft.builtX = draft.countX;
    draft.builtY = draft.countY;
    draft.builtSpacingX = draft.spacingX;
    draft.builtSpacingY = draft.spacingY;
    draft.builtPolar = draft.polar;
    draft.builtCount = draft.count;
    draft.builtSweep = draft.sweepDegrees;
    draft.builtRotate = draft.rotate;
    draft.builtCentre = draft.centre;
    ++draft.builds;
    draft.lines.clear ();
    draft.truncated = false;
    draft.previewError.clear ();
    if (draft.polar) {
        PolarPreview (draft);
        return;
    }

    size_t cells = 0;
    if (!PreviewCells (draft, cells, draft.previewError))
        return;
    const size_t copies = cells - 1;
    if (copies == 0)
        return;
    size_t linesPerCell = 0;
    for (const auto& outline : draft.outlines)
        linesPerCell += outline.points.size () - (outline.closed ? 0 : 1);
    draft.lines.reserve (linesPerCell > kMaxPreviewLines / copies ? kMaxPreviewLines
                                                                  : std::min (kMaxPreviewLines, linesPerCell * copies));
    const size_t countX = size_t (draft.countX), countY = size_t (draft.countY);
    for (size_t y = 0; y < countY; ++y)
        for (size_t x = 0; x < countX; ++x) {
            if (x == 0 && y == 0)
                continue;
            const edit::Vec3 offset { double (x) * draft.spacingX, double (y) * draft.spacingY, 0.0 };
            for (const auto& outline : draft.outlines) {
                const size_t segments = outline.points.size () - (outline.closed ? 0 : 1);
                for (size_t i = 0; i < segments; ++i) {
                    if (draft.lines.size () == kMaxPreviewLines) {
                        draft.truncated = true;
                        return;
                    }
                    draft.lines.push_back (
                        { outline.points[i] + offset, outline.points[(i + 1) % outline.points.size ()] + offset });
                }
            }
        }
}

bool ApplyRectangularArray (Session& s)
{
    if (!CheckRectangularArrayDraft (s))
        return false;
    UpdateRectangularArrayPreview (s); // the centre the preview shows is the one applied
    const auto& draft = *s.rectangularArray;
    if (draft.polar) {
        std::string why;
        if (!PolarPreviewValid (draft, why)) {
            s.message = why;
            return false;
        }
        const size_t count = size_t (draft.count);
        const double sweep = draft.sweepDegrees * kDegrees;
        const bool rotate = draft.rotate;
        const edit::Vec3 centre = draft.centre;
        const auto refs = draft.refs;
        std::vector<edit::Ref> placed;
        const bool applied = s.edits.Apply (
            [&] (edit::Document& doc, std::string& error) {
                return edit::PolarArray (doc, refs, count, centre, sweep, rotate, placed, error);
            },
            false);
        if (!applied && !(count == 1 && s.edits.Message ().empty () && !placed.empty ())) {
            s.message = s.edits.Message ();
            return false;
        }
        s.selection.Set (placed);
        s.rectangularArray.reset ();
        s.message.clear ();
        return true;
    }
    if (draft.countX <= 0 || draft.countY <= 0) {
        s.message = "Array counts must be positive and include the source.";
        return false;
    }
    const size_t countX = size_t (draft.countX), countY = size_t (draft.countY);
    const double spacingX = draft.spacingX, spacingY = draft.spacingY;
    const auto refs = draft.refs;
    const bool noCopies = countX == 1 && countY == 1;
    std::vector<edit::Ref> placed;
    const bool applied = s.edits.Apply (
        [&] (edit::Document& doc, std::string& error) {
            return edit::RectangularArray (doc, refs, countX, countY, spacingX, spacingY, placed, error);
        },
        false);
    // Session::Apply reports an ordinary content no-op as false with no refusal. The 1x1 array is
    // still a successful draft application: it normalizes the source refs and records no history.
    if (!applied && !(noCopies && s.edits.Message ().empty () && !placed.empty ())) {
        s.message = s.edits.Message ();
        return false;
    }
    s.selection.Set (placed);
    s.rectangularArray.reset ();
    s.message.clear ();
    return true;
}

namespace {
// #130: count, angle, turning, and the centre: the gumball pivot by default, picked with snaps by
// repositioning the gumball, or typed.
void PolarArrayControls (Session& s)
{
    auto& draft = *s.rectangularArray;
    ImGui::Text ("Polar array");
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputInt ("Polar count (total)", &draft.count, 1, 4, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputDouble ("Polar angle (degrees)", &draft.sweepDegrees, 15.0, 90.0, "%.2f",
                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Checkbox ("Rotate copies", &draft.rotate);
    ImGui::Checkbox ("Centre at the gumball pivot", &draft.centreAtPivot);
    ImGui::SameLine ();
    ImGui::BeginDisabled (!CanRepositionGumball (s));
    if (ImGui::SmallButton ("Pick centre")) {
        draft.centreAtPivot = true;
        BeginGumballReposition (s);
    }
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        hudshell::HoverTip ("Drag the gumball centre, with snaps, to where the array turns about.");
    ImGui::SetNextItemWidth (150.0f);
    if (LengthInput ({ "Polar centre X", 0.0, 0.0, 0.1, 3 }, draft.centre.x, 0.1, 1.0,
                     ImGuiInputTextFlags_EnterReturnsTrue))
        draft.centreAtPivot = false;
    ImGui::SetNextItemWidth (150.0f);
    if (LengthInput ({ "Polar centre Y", 0.0, 0.0, 0.1, 3 }, draft.centre.y, 0.1, 1.0,
                     ImGuiInputTextFlags_EnterReturnsTrue))
        draft.centreAtPivot = false;
    WrappedHint ("The count includes the source. A full turn (360) spreads it evenly with no copy back on the "
                 "source; a smaller angle puts the last copy at its end. Negative turns clockwise. Or drag the dots "
                 "in the view: the centre moves the axis, the last copy sets the angle.");
    UpdateRectangularArrayPreview (s);
    if (ImGui::SmallButton ("Apply array")) {
        if (ApplyRectangularArray (s))
            return;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Cancel array")) {
        CancelRectangularArray (s);
        return;
    }
    if (!draft.previewError.empty ())
        ImGui::TextWrapped ("%s", draft.previewError.c_str ());
    else if (draft.truncated)
        ImGui::TextWrapped ("Preview is capped at %zu outline segments; Apply still creates every copy.",
                            kMaxPreviewLines);
}
} // namespace

void RectangularArrayControls (Session& s)
{
    if (!s.rectangularArray) {
        const bool objects = !edit::ObjectsOf (Doc (s), s.selection.Items ()).empty ();
        ImGui::BeginDisabled (!objects || s.tool != Tool::Select || s.landscapeMode != LandscapeMode::Select ||
                              s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty () || s.terrainCapture ||
                              s.draftingImport);
        if (ImGui::SmallButton ("Rectangular array..."))
            BeginRectangularArray (s);
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Polar array..."))
            BeginPolarArray (s);
        ImGui::EndDisabled ();
        return;
    }
    if (!CheckRectangularArrayDraft (s))
        return;
    auto& draft = *s.rectangularArray;
    ImGui::Separator ();
    if (draft.polar) {
        PolarArrayControls (s);
        return;
    }
    ImGui::Text ("Rectangular array");
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputInt ("Array count X (total)", &draft.countX, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputInt ("Array count Y (total)", &draft.countY, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    LengthInput ({ "Array spacing X", 0.0, 0.0, 0.1, 3 }, draft.spacingX, 0.1, 1.0,
                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    LengthInput ({ "Array spacing Y", 0.0, 0.0, 0.1, 3 }, draft.spacingY, 0.1, 1.0,
                 ImGuiInputTextFlags_EnterReturnsTrue);
    WrappedHint ("Counts include the source; signed spacing is centre-to-centre. Or drag the dots in the view: "
                 "the first copy along X or Y sets its spacing, the far corner both.");
    UpdateRectangularArrayPreview (s);
    if (ImGui::SmallButton ("Apply array")) {
        if (ApplyRectangularArray (s))
            return;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Cancel array")) {
        CancelRectangularArray (s);
        return;
    }
    if (!draft.previewError.empty ())
        ImGui::TextWrapped ("%s", draft.previewError.c_str ());
    else if (draft.truncated)
        ImGui::TextWrapped ("Preview is capped at %zu outline segments; Apply still creates every cell.",
                            kMaxPreviewLines);
}

namespace {
enum ArrayHandle : int { kSpacingX, kSpacingY, kExtent, kCentre, kAngle, kHandleCount };

// Where handle `h` of the draft is, at the selection's height; false for one it does not have.
bool HandleAt (const RectangularArrayDraft& d, int h, edit::Vec3& at)
{
    const edit::Vec3 base = d.middle;
    if (d.polar) {
        if (h == kCentre) {
            at = { d.centre.x, d.centre.y, base.z };
            return true;
        }
        if (h == kAngle && d.count > 1 && d.previewError.empty ()) {
            const double last = edit::PolarStep (size_t (d.count), d.sweepDegrees * kDegrees) * double (d.count - 1);
            at = edit::TurnInPlan (base, d.centre, last);
            return true;
        }
        return false;
    }
    if (h == kSpacingX && d.countX > 1) {
        at = base + edit::Vec3 { d.spacingX, 0.0, 0.0 };
        return true;
    }
    if (h == kSpacingY && d.countY > 1) {
        at = base + edit::Vec3 { 0.0, d.spacingY, 0.0 };
        return true;
    }
    if (h == kExtent && d.countX > 1 && d.countY > 1) {
        at = base + edit::Vec3 { double (d.countX - 1) * d.spacingX, double (d.countY - 1) * d.spacingY, 0.0 };
        return true;
    }
    return false;
}

// Handle `h` carried to `p`: the field it stands for, set as the drag says.
void DragHandle (RectangularArrayDraft& d, int h, edit::Vec3 p)
{
    constexpr double kLeast = 1.0e-3; // a spacing never dragged to nothing
    const edit::Vec3 base = d.middle;
    const auto spacing = [] (double delta, double cells, double was) {
        const double value = delta / cells;
        return std::abs (value) >= kLeast ? value : was;
    };
    switch (h) {
        case kSpacingX:
            d.spacingX = spacing (p.x - base.x, 1.0, d.spacingX);
            break;
        case kSpacingY:
            d.spacingY = spacing (p.y - base.y, 1.0, d.spacingY);
            break;
        case kExtent:
            d.spacingX = spacing (p.x - base.x, double (d.countX - 1), d.spacingX);
            d.spacingY = spacing (p.y - base.y, double (d.countY - 1), d.spacingY);
            break;
        case kCentre:
            d.centre = { p.x, p.y, d.centre.z };
            d.centreAtPivot = false; // where it was put, not where the gumball is
            break;
        case kAngle: {
            if (std::hypot (p.x - d.centre.x, p.y - d.centre.y) < kLeast)
                break;
            const double from = std::atan2 (base.y - d.centre.y, base.x - d.centre.x);
            double degrees = (std::atan2 (p.y - d.centre.y, p.x - d.centre.x) - from) / kDegrees;
            // The way it turned before: the last copy is put where the pointer is, going round
            // the same way, and the angle ends there (a part turn).
            if (d.sweepDegrees >= 0.0)
                while (degrees <= 0.0)
                    degrees += 360.0;
            else
                while (degrees >= 0.0)
                    degrees -= 360.0;
            while (std::abs (degrees) > 360.0)
                degrees -= std::copysign (360.0, degrees);
            d.sweepDegrees = degrees;
            break;
        }
        default:
            break;
    }
}
} // namespace

bool ArrayHandles (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale)
{
    if (!s.rectangularArray || !CheckRectangularArrayDraft (s))
        return false;
    auto& d = *s.rectangularArray;
    if (d.held >= 0) {
        if (!ImGui::IsMouseDown (ImGuiMouseButton_Left)) {
            d.held = -1;
            return true; // the release is the handle's too
        }
        edit::Vec3 p;
        if (m.Onto (mouse.x, mouse.y, d.middle.z, d.middle, p)) {
            DragHandle (d, d.held, p + d.grab);
            UpdateRectangularArrayPreview (s);
        }
        return true;
    }
    d.hover = -1;
    if (!hovered)
        return false;
    float nearest = 9.0f * scale; // pixels
    for (int h = 0; h < kHandleCount; ++h) {
        edit::Vec3 at;
        ImVec2 pixel;
        if (!HandleAt (d, h, at) || !m.Screen (at, pixel))
            continue;
        if (const float gap = std::hypot (pixel.x - mouse.x, pixel.y - mouse.y); gap < nearest)
            nearest = gap, d.hover = h;
    }
    if (d.hover < 0)
        return false;
    if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) {
        edit::Vec3 at, p;
        HandleAt (d, d.hover, at);
        d.grab = m.Onto (mouse.x, mouse.y, d.middle.z, d.middle, p) ? at - p : edit::Vec3 {};
        d.held = d.hover;
    }
    return true;
}

void DrawRectangularArrayPreview (Session& s, const Map& m, float scale)
{
    if (!CheckRectangularArrayDraft (s))
        return;
    UpdateRectangularArrayPreview (s);
    const auto& draft = *s.rectangularArray;
    for (const auto& line : draft.lines)
        Line (s, m, line.first, line.second, kAccent, 2.0f * scale);
    // #130: the axis shown by its two radii -- to the source and to the last copy -- so the
    // centre and the sweep read without the copies having to.
    if (draft.polar && draft.previewError.empty () && draft.count > 1) {
        const double last = edit::PolarStep (size_t (draft.count), draft.sweepDegrees * kDegrees) * (draft.count - 1);
        const edit::Vec3 centre { draft.centre.x, draft.centre.y, draft.middle.z };
        Line (s, m, centre, draft.middle, kAccent, 1.0f * scale);
        Line (s, m, centre, edit::TurnInPlan (draft.middle, centre, last), kAccent, 1.0f * scale);
    }
    // The handles to drag: filled, larger under the pointer or while held.
    for (int h = 0; h < kHandleCount; ++h)
        if (edit::Vec3 at; HandleAt (draft, h, at)) {
            // ⚠️ NOT THE GUMBALL'S COLOURS (the user, 2026-11-20: "hard to see next to the gumball"):
            // magenta in a white ring, a step larger under the pointer or while held.
            const float radius = (h == draft.hover || h == draft.held ? 7.5f : 5.5f) * scale;
            Dot (s, m, at, radius + 1.5f * scale, IM_COL32 (255, 255, 255, 255), true);
            Dot (s, m, at, radius, kArrayHandle, true);
        }
}

} // namespace geomsrv::archviz::edithud::detail
