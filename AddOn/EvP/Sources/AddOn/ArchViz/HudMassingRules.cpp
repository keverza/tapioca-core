#include "ArchViz/HudMassingRules.hpp"
#include "ArchViz/HudMassingLabels.hpp"
#include "ArchViz/HudMassingDiagram.hpp"
#include "ArchViz/HudShell.hpp"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace geomsrv::archviz::hudmassingrules {
namespace {
namespace rules = massingrules;
using Point = std::pair<double, double>;

std::vector<Point> Points (const rules::Edge& edge)
{
    if (std::abs (edge.arcAngle) < 1e-8)
        return { { edge.ax, edge.ay }, { edge.bx, edge.by } };
    const double dx = edge.bx - edge.ax, dy = edge.by - edge.ay;
    const double shift = 0.5 / std::tan (edge.arcAngle * 0.5);
    const double cx = (edge.ax + edge.bx) * 0.5 - dy * shift;
    const double cy = (edge.ay + edge.by) * 0.5 + dx * shift;
    const int count = (std::clamp) (int (std::ceil (std::abs (edge.arcAngle) * 24)), 2, 151);
    std::vector<Point> points;
    for (int i = 0; i <= count; ++i) {
        const double angle = edge.arcAngle * i / count;
        points.emplace_back (cx + (edge.ax - cx) * std::cos (angle) - (edge.ay - cy) * std::sin (angle),
                             cy + (edge.ax - cx) * std::sin (angle) + (edge.ay - cy) * std::cos (angle));
    }
    return points;
}

massingcalculation::Request Inputs (const rules::Page& page, const Draft& draft);

bool Distance (const char* label, double& distance, Draft& draft, double low = 0, double high = 1000)
{
    ImGui::PushID (label);
    ImGui::SetNextItemWidth (140);
    const bool changed = std::string (label) == "Cap Project Z"
                             ? ImGui::SliderScalar (label, ImGuiDataType_Double, &distance, &low, &high, "%.2f m",
                                                    ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)
                             : ImGui::DragScalar (label, ImGuiDataType_Double, &distance, 0.01f, &low, &high, "%.2f m",
                                                  ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp);
    if (changed)
        distance = std::round (distance * 100) / 100;
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Set"))
        draft.numbers.push_back ({ Inputs (draft.source, draft), label, distance, low, high, draft.selected });
    ImGui::PopID ();
    return changed;
}

void EdgeControls (Draft& draft)
{
    auto& assignment = draft.assignments[size_t (draft.selected)];
    const char* labels[] = { "0m", "1m", "3m", "Custom" };
    const int presets[] = { 0, 1, 3, -1 };
    const double distance = assignment.distance;
    for (int i = 0; i < 4; ++i) {
        if (i)
            ImGui::SameLine ();
        const bool selected = presets[i] < 0 ? distance != 0 && distance != 1 && distance != 3 : distance == presets[i];
        if (selected)
            ImGui::PushStyleColor (ImGuiCol_Button, ImGui::GetStyleColorVec4 (ImGuiCol_ButtonActive));
        if (ImGui::Button (labels[i]))
            SelectOffset (draft, presets[i]);
        if (selected)
            ImGui::PopStyleColor ();
    }
    ImGui::Text ("Offset: %.2f m", assignment.distance);
    if (assignment.review) {
        ImGui::TextWrapped ("This segment needs assignment review.");
        if (ImGui::SmallButton ("Confirm this assignment")) {
            assignment.review = false;
            draft.dirty = true;
        }
    }
}

massingcalculation::Request Inputs (const rules::Page& page, const Draft& draft)
{
    auto request = draft.calculation;
    request.before = page;
    request.assignments = draft.assignments;
    request.endpoints = draft.endpoints;
    request.regulated = draft.regulated;
    return request;
}

void Follow (const rules::Page& page, Draft& draft)
{
    auto request = Inputs (page, draft);
    if (!draft.lastRequested || !massingcalculation::SameRequest (*draft.lastRequested, request)) {
        draft.lastRequested = request;
        draft.calculations.push_back (std::move (request));
    }
}

std::vector<rules::Edit> Diagram (const rules::Page& page, Draft& draft,
                                  const std::shared_ptr<const massingcalculation::Preview>& preview,
                                  const std::vector<rules::Page>& parcels,
                                  const std::shared_ptr<const massingcalculation::Preview>& sitePreview)
{
    std::vector<rules::Edit> edits;
    std::vector<std::vector<Point>> paths;
    double minX = page.edges[0].ax, maxX = minX, minY = page.edges[0].ay, maxY = minY;
    for (const auto& edge : page.edges) {
        paths.push_back (Points (edge));
        for (const auto& p : paths.back ()) {
            minX = (std::min) (minX, p.first);
            maxX = (std::max) (maxX, p.first);
            minY = (std::min) (minY, p.second);
            maxY = (std::max) (maxY, p.second);
        }
    }
    for (const auto& parcel : parcels)
        for (const auto& edge : parcel.edges)
            for (const auto& p : Points (edge)) {
                minX = (std::min) (minX, p.first);
                maxX = (std::max) (maxX, p.first);
                minY = (std::min) (minY, p.second);
                maxY = (std::max) (maxY, p.second);
            }
    const ImVec2 origin = ImGui::GetCursorScreenPos ();
    const ImVec2 extent ((std::max) (80.0f, ImGui::GetContentRegionAvail ().x), 200);
    const float margin =
        (std::min) ({ (std::max) (32.0f, (std::min) (48.0f, 2 * ImGui::GetFontSize ())), extent.x / 4, extent.y / 4 });
    const double factor = (std::min) ((extent.x - 2 * margin) / (std::max) (maxX - minX, 1e-9),
                                      (extent.y - 2 * margin) / (std::max) (maxY - minY, 1e-9));
    const auto project = [&] (Point p) {
        return ImVec2 (origin.x + extent.x * 0.5f + float ((p.first - (minX + maxX) * 0.5) * factor),
                       origin.y + extent.y * 0.5f - float ((p.second - (minY + maxY) * 0.5) * factor));
    };
    ImGui::InvisibleButton ("##massing.site", extent,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered ();
    auto* draw = ImGui::GetWindowDrawList ();
    ProjectedDrawList occupied;
    std::vector<DiagramLabel> segmentLabels, otherLabels;
    const auto addLine = [&] (ImVec2 from, ImVec2 to, ImU32 rgba, float width) {
        draw->AddLine (from, to, rgba, width);
        if (draft.labels)
            occupied.lines.push_back ({ { from.x, from.y }, { to.x, to.y }, rgba, width });
    };
    const auto addOffset = [&] (const std::vector<double>& xy) {
        std::vector<ScreenPoint> points;
        for (size_t i = 0; i + 1 < xy.size (); i += 2) {
            const auto at = project ({ xy[i], xy[i + 1] });
            points.push_back ({ at.x, at.y });
        }
        DrawDiagramOffset (*draw, points, ImGui::GetFontSize (), draft.labels ? &occupied : nullptr);
    };
    draw->AddRectFilled (origin, { origin.x + extent.x, origin.y + extent.y }, ImGui::GetColorU32 (ImGuiCol_FrameBg),
                         4);
    draw->PushClipRect (origin, { origin.x + extent.x, origin.y + extent.y }, true);
    std::string otherHit;
    float nearestOther = 100;
    const ImVec2 pointer = ImGui::GetIO ().MousePos;
    for (size_t k = 0; k < parcels.size (); ++k) {
        const auto& parcel = parcels[k];
        if (parcel.guid == page.guid || parcel.edges.empty ())
            continue;
        for (const auto& edge : parcel.edges) {
            const auto path = Points (edge);
            for (size_t i = 1; i < path.size (); ++i) {
                const auto from = project (path[i - 1]), to = project (path[i]);
                addLine (from, to, IM_COL32 (170, 68, 101, 255), 1.5f);
                const float dx = to.x - from.x, dy = to.y - from.y;
                const float px = pointer.x - from.x, py = pointer.y - from.y;
                const float squared = dx * dx + dy * dy;
                const float t = squared > 0 ? (std::clamp) ((px * dx + py * dy) / squared, 0.0f, 1.0f) : 0;
                const float distance = (px - t * dx) * (px - t * dx) + (py - t * dy) * (py - t * dy);
                if (distance < nearestOther) {
                    nearestOther = distance;
                    otherHit = parcel.guid;
                }
            }
        }
        if (draft.labels) {
            const auto at = project ({ parcel.edges[0].ax, parcel.edges[0].ay });
            const auto label = "Parcel " + std::to_string (k + 1);
            otherLabels.push_back ({ { at.x, at.y }, { 1, 0 }, label, ImGui::GetColorU32 (ImGuiCol_Text) });
        }
    }
    if (sitePreview)
        for (const auto& parcel : sitePreview->parcels) {
            if (parcel.inputs.before.guid == page.guid)
                continue;
            addOffset (parcel.result.offsetXY);
        }
    const auto* inputs = preview ? &preview->inputs : nullptr;
    const auto* result = preview ? &preview->result : nullptr;
    if (preview)
        for (const auto& parcel : preview->parcels)
            if (parcel.inputs.before.guid == page.guid) {
                inputs = &parcel.inputs;
                result = &parcel.result;
                break;
            }
    if (inputs && massingcalculation::SameRequest (*inputs, Inputs (page, draft))) {
        addOffset (result->offsetXY);
    }
    int pointHit = -1, edgeHit = -1;
    float nearestPoint = 81, nearestEdge = 100;
    for (size_t i = 0; i < paths.size (); ++i) {
        const ImVec2 a = project (paths[i][0]);
        const float dx = pointer.x - a.x, dy = pointer.y - a.y;
        if (dx * dx + dy * dy < nearestPoint) {
            nearestPoint = dx * dx + dy * dy;
            pointHit = int (i);
        }
        for (size_t p = 1; p < paths[i].size (); ++p) {
            const ImVec2 from = project (paths[i][p - 1]), to = project (paths[i][p]);
            addLine (from, to, IM_COL32 (170, 68, 101, 255), int (i) == draft.selected ? 3.0f : 1.5f);
            const float ex = to.x - from.x, ey = to.y - from.y;
            const float px = pointer.x - from.x, py = pointer.y - from.y;
            const float length = ex * ex + ey * ey;
            const float t = length > 0 ? (std::clamp) ((px * ex + py * ey) / length, 0.0f, 1.0f) : 0;
            const float distance = (px - t * ex) * (px - t * ex) + (py - t * ey) * (py - t * ey);
            if (distance < nearestEdge) {
                nearestEdge = distance;
                edgeHit = int (i);
            }
        }
        draw->AddCircleFilled (a, 4, draft.endpoints[i] ? IM_COL32 (47, 111, 235, 255) : IM_COL32 (170, 180, 185, 255));
        if (draft.labels)
            occupied.lines.push_back ({ { a.x, a.y }, { a.x, a.y } }); // Five-pixel clearance covers the point marker.
        const size_t middle = paths[i].size () / 2;
        const auto mid = project (paths[i].size () == 2 ? Point { (page.edges[i].ax + page.edges[i].bx) * 0.5,
                                                                  (page.edges[i].ay + page.edges[i].by) * 0.5 }
                                                        : paths[i][middle]);
        if (draft.labels) {
            const std::string point = "P" + std::to_string (i + 1);
            otherLabels.push_back ({ { a.x, a.y }, { 1, 0 }, point, ImGui::GetColorU32 (ImGuiCol_TextDisabled) });
            char caption[128];
            std::snprintf (caption, sizeof (caption), "S%zu%s  %.2f m", i + 1, draft.assignments[i].review ? " !" : "",
                           draft.assignments[i].distance);
            const auto before = project (paths[i][middle - 1]);
            const auto after = project (paths[i][(std::min) (middle + 1, paths[i].size () - 1)]);
            segmentLabels.push_back ({ { mid.x, mid.y },
                                       { after.x - before.x, after.y - before.y },
                                       caption,
                                       ImGui::GetColorU32 (ImGuiCol_Text) });
        }
    }
    if (draft.labels) {
        const ScreenTextMeasure measure = [] (std::string_view text, float fontSize, ScreenTextExtent& extent) {
            const auto size =
                ImGui::GetFont ()->CalcTextSizeA (fontSize, FLT_MAX, 0, text.data (), text.data () + text.size ());
            extent = { size.x, size.y };
            return true;
        };
        size_t labelWork = 0;
        const auto place = [&] (const DiagramLabel& input) {
            labelWork += 24 * (occupied.lines.size () + occupied.labels.size ());
            if (labelWork > 1000000)
                return; // Dense sites keep interactive geometry and full hover values, not a long layout stall.
            if (const auto label =
                    PlaceDiagramLabel (occupied, input, { origin.x, origin.y },
                                       { origin.x + extent.x, origin.y + extent.y }, ImGui::GetFontSize (), measure))
                DrawDiagramLabel (*draw, *label);
        };
        // The selected segment gets first choice, then stable contour order, then point/parcel IDs.
        place (segmentLabels[size_t (draft.selected)]);
        for (size_t i = 0; i < segmentLabels.size (); ++i)
            if (int (i) != draft.selected)
                place (segmentLabels[i]);
        for (const auto& label : otherLabels)
            place (label);
    }
    draw->PopClipRect ();
    if (hovered) {
        if (edgeHit >= 0 && !ImGui::IsPopupOpen ("##massing.site.context")) {
            ImGui::SetTooltip ("Segment S%d\nOffset: %.2f m", edgeHit + 1,
                               draft.assignments[size_t (edgeHit)].distance);
        }
        if (pointHit >= 0 || edgeHit >= 0 || !otherHit.empty ())
            ImGui::SetMouseCursor (ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseReleased (ImGuiMouseButton_Left) && edgeHit >= 0)
            draft.selected = edgeHit;
        else if (ImGui::IsMouseReleased (ImGuiMouseButton_Left) && !otherHit.empty ())
            draft.pickedParcel = otherHit;
        if (ImGui::IsMouseReleased (ImGuiMouseButton_Right)) {
            hudshell::ClaimRightClick ();
            draft.targetPoint = pointHit;
            draft.targetEdge = pointHit < 0 ? edgeHit : -1;
            if (pointHit < 0 && edgeHit >= 0)
                draft.selected = edgeHit;
            ImGui::OpenPopup ("##massing.site.context");
        }
    }
    if (ImGui::BeginPopup ("##massing.site.context")) {
        if (draft.targetPoint >= 0 && size_t (draft.targetPoint) < draft.endpoints.size ()) {
            bool used = draft.endpoints[size_t (draft.targetPoint)];
            ImGui::Text ("Endpoint %d", draft.targetPoint + 1);
            if (ImGui::Checkbox ("Use for average elevation", &used))
                draft.endpoints[size_t (draft.targetPoint)] = used;
            ImGui::TextDisabled ("Run-local; preview updates automatically.");
        }
        else if (draft.targetEdge >= 0) {
            ImGui::Text ("Segment %d", draft.selected + 1);
            EdgeControls (draft);
            ImGui::TextDisabled ("0m: vertical edge, no height slope.");
            bool regulated = draft.regulated[size_t (draft.selected)];
            if (ImGui::Checkbox ("STR height regulation", &regulated))
                draft.regulated[size_t (draft.selected)] = regulated;
            ImGui::TextDisabled ("Unchecked: height-unregulated NONE (also no offset).");
        }
        ImGui::Separator ();
        if (ImGui::BeginMenu ("Parcel settings")) {
            if (Distance ("Default setback", draft.defaultDistance, draft)) {
                for (auto& assignment : draft.assignments)
                    if (assignment.mode == rules::Mode::Default)
                        assignment.distance = draft.defaultDistance;
                draft.dirty = true;
            }
            auto& calculation = draft.calculation;
            int landscape = calculation.landscape;
            const char* terrains[] = { "Existing terrain", "New terrain" };
            if (ImGui::Combo ("Landscape", &landscape, terrains, 2))
                calculation.landscape = landscape;
            Distance ("STR base height", calculation.baseHeight, draft);
            Distance ("Run per 1 m rise", calculation.runPerRise, draft, 0.01);
            Distance ("Flat base depth", calculation.baseDepth, draft, 0.01, 100);
            ImGui::EndMenu ();
        }
        if (ImGui::MenuItem ("Save assignments", nullptr, false, draft.dirty)) {
            metadata::Property property;
            if (rules::Encode (page.edges, draft.assignments, property, draft.note)) {
                if (page.hasStored && property.value == page.stored)
                    draft.dirty = false;
                else
                    edits.push_back ({ page, draft.assignments });
            }
        }
        if (ImGui::MenuItem ("Discard edits", nullptr, false, draft.dirty)) {
            draft.assignments = page.assignments;
            draft.dirty = false;
            draft.note.clear ();
            draft.defaultDistance = 3;
            for (const auto& assignment : page.assignments)
                if (assignment.mode == rules::Mode::Default) {
                    draft.defaultDistance = assignment.distance;
                    break;
                }
        }
        ImGui::TextDisabled ("Automatic preview; Save only persists offsets.");
        ImGui::EndPopup ();
    }
    return edits;
}
} // namespace

void Sync (const rules::Page& page, Draft& draft)
{
    if (draft.source.guid == page.guid && draft.source.known == page.known &&
        rules::SameGeometry (draft.source.edges, page.edges) && draft.source.hasStored == page.hasStored &&
        draft.source.stored == page.stored)
        return;
    const bool sameParcel = draft.source.guid == page.guid;
    const bool sameStored = draft.source.hasStored == page.hasStored && draft.source.stored == page.stored;
    if (sameParcel && sameStored && draft.source.known && page.known &&
        !rules::SameGeometry (draft.source.edges, page.edges) &&
        draft.assignments.size () == draft.source.edges.size ()) {
        const auto mapping = rules::SegmentMap (draft.source.edges, page.edges);
        const auto previous = draft;
        draft.source = page;
        draft.assignments = page.assignments;
        draft.endpoints.assign (page.edges.size (), true);
        draft.regulated.assign (page.edges.size (), true);
        draft.lastRequested.reset ();
        draft.numbers.clear (); // Old geometry prompts must never target a remapped segment.
        draft.calculations.clear ();
        draft.targetEdge = draft.targetPoint = -1;
        draft.selected = 0;
        for (size_t i = 0; i < mapping.size (); ++i) {
            if (mapping[i] < 0)
                continue;
            const size_t j = size_t (mapping[i]);
            draft.assignments[i] = previous.assignments[j];
            if (j < previous.regulated.size ())
                draft.regulated[i] = previous.regulated[j];
            const auto& old = previous.source.edges[j];
            const auto& now = page.edges[i];
            const size_t endpoint =
                std::hypot (now.ax - old.ax, now.ay - old.ay) <= std::hypot (now.ax - old.bx, now.ay - old.by)
                    ? j
                    : (j + 1) % previous.source.edges.size ();
            if (endpoint < previous.endpoints.size ())
                draft.endpoints[i] = previous.endpoints[endpoint];
            if (mapping[i] == previous.selected)
                draft.selected = int (i);
        }
        draft.dirty = true; // Save will persist new fingerprints; preview is valid immediately.
        draft.note = "Property line changed; existing segment offsets retained.";
        return;
    }
    metadata::Property authored;
    std::string error;
    const bool sameGeometry = draft.source.guid == page.guid && rules::SameGeometry (draft.source.edges, page.edges);
    const bool saved = sameGeometry && draft.dirty && page.hasStored &&
                       rules::Encode (draft.source.edges, draft.assignments, authored, error) &&
                       authored.value == page.stored;
    const auto endpoints = draft.endpoints;
    const auto regulated = draft.regulated;
    const auto calculation = draft.calculation;
    const bool labels = draft.labels;
    const bool dimensions = draft.offsetDimensions;
    const bool discarded = draft.dirty && !saved;
    draft = {};
    draft.source = page;
    draft.labels = labels;
    draft.offsetDimensions = dimensions;
    draft.assignments = page.assignments;
    draft.endpoints.resize (page.edges.size (), true);
    draft.regulated.resize (page.edges.size (), true);
    if (sameGeometry && endpoints.size () == page.edges.size ())
        draft.endpoints = endpoints;
    if (sameGeometry && regulated.size () == page.edges.size ()) {
        draft.regulated = regulated;
    }
    if (sameParcel)
        draft.calculation = calculation;
    for (const auto& assignment : page.assignments)
        if (assignment.mode == rules::Mode::Default) {
            draft.defaultDistance = assignment.distance;
            break;
        }
    if (discarded)
        draft.note = "Unsaved draft discarded because the property line or saved assignments changed.";
}

std::vector<rules::Edit> Draw (const rules::Page& page, Draft& draft, bool busy, const std::string& calculationNote,
                               const std::shared_ptr<const massingcalculation::Preview>& preview,
                               const std::vector<rules::Page>& parcels,
                               const std::shared_ptr<const massingcalculation::Preview>& sitePreview)
{
    Sync (page, draft);
    std::vector<rules::Edit> edits;
    if (!ImGui::CollapsingHeader ("Property line rules", ImGuiTreeNodeFlags_DefaultOpen)) {
        Follow (page, draft);
        return edits;
    }
    if (!page.note.empty () && (!page.known || std::any_of (draft.assignments.begin (), draft.assignments.end (),
                                                            [] (const auto& a) { return a.review; })))
        ImGui::TextWrapped ("%s", page.note.c_str ());
    if (!calculationNote.empty ())
        ImGui::TextWrapped ("%s", calculationNote.c_str ());
    else if (busy)
        ImGui::TextDisabled ("Updating preview...");
    if (!page.known || page.edges.empty () || draft.assignments.size () != page.edges.size ()) {
        if (page.note.empty ())
            ImGui::TextDisabled ("Define closed property-line Polylines first.");
        Follow (page, draft);
        return edits;
    }
    ImGui::PushID ("massing.rules");
    ImGui::PushID (page.guid.c_str ());
    draft.selected = (std::clamp) (draft.selected, 0, int (page.edges.size ()) - 1);
    if (!draft.note.empty ())
        ImGui::TextWrapped ("%s", draft.note.c_str ());
    ImGui::Checkbox ("Show segment, point and dimension text", &draft.labels);
    ImGui::Checkbox ("Show offset dimensions in overlay", &draft.offsetDimensions);
    ImGui::Checkbox ("Project height cap", &draft.calculation.capped);
    if (draft.calculation.capped)
        Distance ("Cap Project Z", draft.calculation.capZ, draft, 5, 50);
    edits = Diagram (page, draft, preview, parcels, sitePreview);
    ImGui::PopID ();
    ImGui::PopID ();
    Follow (page, draft);
    return edits;
}

bool AnswerNumber (Draft& draft, const NumberEdit& edit, double number)
{
    if (!std::isfinite (number) || number < edit.min || number > edit.max ||
        !massingcalculation::SameRequest (edit.before, Inputs (draft.source, draft)))
        return false;
    if (edit.key == "Cap Project Z")
        draft.calculation.capZ = number;
    else if (edit.key == "STR base height")
        draft.calculation.baseHeight = number;
    else if (edit.key == "Run per 1 m rise")
        draft.calculation.runPerRise = number;
    else if (edit.key == "Flat base depth")
        draft.calculation.baseDepth = number;
    else if (edit.key == "Default setback") {
        draft.defaultDistance = number;
        for (auto& assignment : draft.assignments)
            if (assignment.mode == rules::Mode::Default)
                assignment.distance = number;
        draft.dirty = true;
    }
    else if (edit.key == "Offset" && edit.edge >= 0 && size_t (edit.edge) < draft.assignments.size ()) {
        draft.assignments[size_t (edit.edge)].mode = rules::Mode::Custom;
        draft.assignments[size_t (edit.edge)].distance = number;
        draft.assignments[size_t (edit.edge)].review = false;
        draft.dirty = true;
    }
    else
        return false;
    return true;
}

void SelectOffset (Draft& draft, int preset)
{
    if (draft.selected < 0 || size_t (draft.selected) >= draft.assignments.size ())
        return;
    auto& assignment = draft.assignments[size_t (draft.selected)];
    if (preset == -1) {
        draft.numbers.push_back (
            { Inputs (draft.source, draft), "Offset", assignment.distance, 0, 1000, draft.selected });
        return; // Cancel leaves both the value and mode unchanged.
    }
    if (preset != 0 && preset != 1 && preset != 3)
        return;
    assignment.mode = preset == 0 ? rules::Mode::None : rules::Mode::Custom;
    assignment.distance = preset;
    assignment.review = false;
    draft.dirty = true;
}
} // namespace geomsrv::archviz::hudmassingrules
