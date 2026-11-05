#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud::detail {
namespace {

constexpr size_t kMaxPreviewLines = 20000;
constexpr double kPreviewMaxAngle = 0.2;

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
    return draft.builtX == draft.countX && draft.builtY == draft.countY &&
           SameNumber (draft.builtSpacingX, draft.spacingX) && SameNumber (draft.builtSpacingY, draft.spacingY);
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

bool CheckRectangularArrayDraft (Session& s)
{
    if (!s.rectangularArray)
        return false;
    const auto& draft = *s.rectangularArray;
    const bool current = draft.revision == Doc (s).revision && draft.refs == s.selection.Items () &&
                         draft.tool == s.tool && draft.landscapeMode == s.landscapeMode;
    if (current)
        return true;
    s.rectangularArray.reset ();
    s.message = "Rectangular array draft cancelled: its selection, document or tool changed.";
    return false;
}

void CancelRectangularArray (Session& s)
{
    s.rectangularArray.reset ();
    s.message.clear ();
}

bool BeginRectangularArray (Session& s)
{
    if (s.rectangularArray && CheckRectangularArrayDraft (s))
        return true;
    if (s.tool != Tool::Select || s.landscapeMode != LandscapeMode::Select || s.edits.InGesture () ||
        GizmoModeLocked (s) || !s.drawing.empty () || s.terrainCapture || s.draftingImport) {
        s.message = "Finish the current input or Esc it, then start the rectangular array from Select.";
        return false;
    }
    const edit::Clipboard clip = edit::Copy (Doc (s), s.selection.Items ());
    if (clip.Empty ()) {
        s.message = "Select the objects to array.";
        return false;
    }

    RectangularArrayDraft draft;
    draft.refs = s.selection.Items ();
    draft.revision = Doc (s).revision;
    draft.tool = s.tool;
    draft.landscapeMode = s.landscapeMode;
    std::string error;
    for (const auto& path : clip.paths) {
        RectangularArrayDraft::Outline outline;
        if (!PathOutline (path, outline, error)) {
            s.message = "Rectangular array preview refused: " + error;
            return false;
        }
        draft.outlines.push_back (std::move (outline));
    }
    for (const auto& entity : clip.entities) {
        RectangularArrayDraft::Outline outline;
        outline.points = edit::Shape (entity, kPreviewMaxAngle);
        outline.closed = entity.closed;
        if (outline.points.size () < 2) {
            s.message = "Rectangular array preview needs an outline for every selected object.";
            return false;
        }
        draft.outlines.push_back (std::move (outline));
    }
    s.rectangularArray = std::move (draft);
    s.message.clear ();
    UpdateRectangularArrayPreview (s);
    return true;
}

void UpdateRectangularArrayPreview (Session& s)
{
    if (!CheckRectangularArrayDraft (s))
        return;
    auto& draft = *s.rectangularArray;
    if (Cached (draft))
        return;
    draft.builtX = draft.countX;
    draft.builtY = draft.countY;
    draft.builtSpacingX = draft.spacingX;
    draft.builtSpacingY = draft.spacingY;
    ++draft.builds;
    draft.lines.clear ();
    draft.truncated = false;
    draft.previewError.clear ();

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
    const auto& draft = *s.rectangularArray;
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

void RectangularArrayControls (Session& s)
{
    if (!s.rectangularArray) {
        const bool objects = !edit::ObjectsOf (Doc (s), s.selection.Items ()).empty ();
        ImGui::BeginDisabled (!objects || s.tool != Tool::Select || s.landscapeMode != LandscapeMode::Select ||
                              s.edits.InGesture () || GizmoModeLocked (s) || !s.drawing.empty () || s.terrainCapture ||
                              s.draftingImport);
        if (ImGui::SmallButton ("Rectangular array..."))
            BeginRectangularArray (s);
        ImGui::EndDisabled ();
        return;
    }
    if (!CheckRectangularArrayDraft (s))
        return;
    auto& draft = *s.rectangularArray;
    ImGui::Separator ();
    ImGui::Text ("Rectangular array");
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputInt ("Array count X (total)", &draft.countX, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputInt ("Array count Y (total)", &draft.countY, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputDouble ("Array spacing X (m)", &draft.spacingX, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth (150.0f);
    ImGui::InputDouble ("Array spacing Y (m)", &draft.spacingY, 0.1, 1.0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled ("Counts include the source; signed spacing is centre-to-centre.");
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

void DrawRectangularArrayPreview (Session& s, const Map& m, float scale)
{
    if (!CheckRectangularArrayDraft (s))
        return;
    UpdateRectangularArrayPreview (s);
    const auto& draft = *s.rectangularArray;
    for (const auto& line : draft.lines)
        Line (s, m, line.first, line.second, kAccent, 2.0f * scale);
}

} // namespace geomsrv::archviz::edithud::detail
