#include "ArchViz/HudMassingRules.hpp"
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

void SetMode (Draft& draft, int mode)
{
    auto& assignment = draft.assignments[size_t (draft.selected)];
    assignment.mode = rules::Mode (mode);
    if (mode != 1)
        assignment.distance = mode == 2 ? 0 : draft.defaultDistance;
    assignment.review = false;
    draft.dirty = true;
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
    int mode = int (assignment.mode);
    const char* names[] = { "Default", "Custom", "Road / None (vertical)" };
    ImGui::SetNextItemWidth (140);
    if (ImGui::Combo ("Offset mode", &mode, names, 3))
        SetMode (draft, mode);
    if (assignment.mode == rules::Mode::Custom) {
        if (Distance ("Offset", assignment.distance, draft)) {
            assignment.review = false;
            draft.dirty = true;
        }
    }
    else
        ImGui::Text ("Offset: %.2f m", assignment.distance);
    if (ImGui::SmallButton ("Road: 1 m")) {
        SetMode (draft, 1);
        assignment.distance = 1;
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Road: no setback"))
        SetMode (draft, 2);
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
    const double factor =
        (std::min) ((extent.x - 32) / (std::max) (maxX - minX, 1e-9), (extent.y - 32) / (std::max) (maxY - minY, 1e-9));
    const auto project = [&] (Point p) {
        return ImVec2 (origin.x + extent.x * 0.5f + float ((p.first - (minX + maxX) * 0.5) * factor),
                       origin.y + extent.y * 0.5f - float ((p.second - (minY + maxY) * 0.5) * factor));
    };
    ImGui::InvisibleButton ("##massing.site", extent,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered ();
    auto* draw = ImGui::GetWindowDrawList ();
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
                draw->AddLine (from, to, IM_COL32 (170, 68, 101, 255), 1.5f);
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
            draw->AddText ({ at.x + 5, at.y - ImGui::GetTextLineHeight () }, ImGui::GetColorU32 (ImGuiCol_Text),
                           label.c_str ());
        }
    }
    if (sitePreview)
        for (const auto& parcel : sitePreview->parcels) {
            if (parcel.inputs.before.guid == page.guid)
                continue;
            const auto& xy = parcel.result.offsetXY;
            for (size_t i = 0; i + 1 < xy.size (); i += 2) {
                const size_t next = (i + 2) % xy.size ();
                draw->AddLine (project ({ xy[i], xy[i + 1] }), project ({ xy[next], xy[next + 1] }),
                               IM_COL32 (166, 98, 38, 255), 2);
            }
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
        const auto& xy = result->offsetXY;
        for (size_t i = 0; i + 1 < xy.size (); i += 2) {
            const size_t next = (i + 2) % xy.size ();
            draw->AddLine (project ({ xy[i], xy[i + 1] }), project ({ xy[next], xy[next + 1] }),
                           IM_COL32 (166, 98, 38, 255), 2);
        }
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
            draw->AddLine (from, to, IM_COL32 (170, 68, 101, 255), int (i) == draft.selected ? 3.0f : 1.5f);
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
        const size_t middle = paths[i].size () / 2;
        const auto mid = project (paths[i].size () == 2 ? Point { (page.edges[i].ax + page.edges[i].bx) * 0.5,
                                                                  (page.edges[i].ay + page.edges[i].by) * 0.5 }
                                                        : paths[i][middle]);
        const std::string label = "S" + std::to_string (i + 1) + (draft.assignments[i].review ? " !" : "");
        if (draft.labels) {
            draw->AddText ({ mid.x + 3, mid.y }, ImGui::GetColorU32 (ImGuiCol_Text), label.c_str ());
            const std::string point = "P" + std::to_string (i + 1);
            draw->AddText ({ a.x + 5, a.y + 4 }, ImGui::GetColorU32 (ImGuiCol_TextDisabled), point.c_str ());
            double length = 0;
            for (size_t j = 1; j < paths[i].size (); ++j)
                length +=
                    std::hypot (paths[i][j].first - paths[i][j - 1].first, paths[i][j].second - paths[i][j - 1].second);
            char dimension[48];
            std::snprintf (dimension, sizeof (dimension), "%.2f m", length);
            draw->AddText ({ mid.x + 3, mid.y + ImGui::GetTextLineHeight () },
                           ImGui::GetColorU32 (ImGuiCol_TextDisabled), dimension);
        }
    }
    draw->PopClipRect ();
    if (hovered) {
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
            ImGui::TextDisabled ("Road / None: vertical edge, no height slope.");
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
        draft.calculation = calculation;
    }
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
    if (!page.note.empty ())
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
        draft.assignments[size_t (edit.edge)].distance = number;
        draft.dirty = true;
    }
    else
        return false;
    return true;
}
} // namespace geomsrv::archviz::hudmassingrules
