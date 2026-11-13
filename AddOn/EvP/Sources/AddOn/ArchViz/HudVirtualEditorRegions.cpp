#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorMaterial.hpp"
#include "ArchViz/HudSlopeFields.hpp"
#include <imgui_internal.h> // ImGui::GetActiveID
#include <algorithm>
#include <set>
#include <utility>

// OVERLAY-INVARIANTS.md §§8,14: model edits stay in the existing session/history;
// this inspector changes no input ownership, renderer or native-write lifecycle.
namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;
namespace {
// Even-odd containment over all rings; region rings are one outer plus holes, so no
// orientation is needed. A face seed inside an object's evaluated footprint region
// counts that object as a candidate owner of the selected face.
bool FaceInside (edit::Vec3 point, const std::vector<std::vector<edit::Vec3>>& rings)
{
    bool inside = false;
    for (const auto& ring : rings)
        for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];
            if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
    return inside;
}

bool Slider (Session& s, lg::Id object, const char* label, double& value, double lo, double hi, const char* format,
             bool& release)
{
    const auto id = ImGui::GetID (label);
    if (s.regionSliderId == id && s.regionSliderObject == object) {
        lo = s.regionSliderMin;
        hi = s.regionSliderMax;
    }
    ImGui::SliderScalar (label, ImGuiDataType_Double, &value, &lo, &hi, format, ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActivated () && !s.edits.InGesture () && s.edits.Begin ()) {
        s.regionSliderObject = object;
        s.regionSliderId = id;
        s.regionSliderMin = lo;
        s.regionSliderMax = hi;
    }
    release |= ImGui::IsItemDeactivated () && s.regionSliderObject == object;
    return ImGui::IsItemEdited ();
}

bool PointFields (Session& s, lg::Id object, const char* label, edit::Vec3& point, bool elevation, bool& release)
{
    ImGui::PushID (label);
    ImGui::TextUnformatted (label);
    bool changed = Slider (s, object, "X (m)", point.x, point.x - 10, point.x + 10, "%.3f", release);
    changed |= Slider (s, object, "Y (m)", point.y, point.y - 10, point.y + 10, "%.3f", release);
    if (elevation)
        changed |= Slider (s, object, "Z (m)", point.z, point.z - 10, point.z + 10, "%.3f", release);
    ImGui::PopID ();
    return changed;
}

bool SelectedSlopePoints (const Session& s, edit::Vec3& first, edit::Vec3& second)
{
    std::vector<edit::Vec3> points;
    std::vector<edit::Ref> seen;
    for (const auto& ref : s.selection.Items ()) {
        if (ref.part != edit::Part::Vertex || std::find (seen.begin (), seen.end (), ref) != seen.end ())
            continue;
        const auto* entity = edit::Find (s.edits.Doc (), ref.entity);
        const int index = entity ? edit::VertexIndex (*entity, ref.item) : -1;
        if (index >= 0) {
            points.push_back (entity->vertices[static_cast<size_t> (index)].p);
            seen.push_back (ref);
        }
    }
    if (points.size () != 2)
        return false;
    first = points[0];
    second = points[1];
    return true;
}

constexpr SlopeFieldLabels s_cutSlopeLabels {
    "Cut face unit",
    "Cut face (%)",
    "Cut face (degrees)",
    "Cut rise",
    "Cut run",
    "Cut height (m)",
    "Cut plan run (m)",
    "Cut grade: >0 to 1000%; vertical is not supported. Height/run uses model metres."
};
constexpr SlopeFieldLabels s_fillSlopeLabels {
    "Fill face unit",
    "Fill face (%)",
    "Fill face (degrees)",
    "Fill rise",
    "Fill run",
    "Fill height (m)",
    "Fill plan run (m)",
    "Fill grade: >0 to 1000%; vertical is not supported. Height/run uses model metres."
};
constexpr SlopeFieldLabels s_contourSlopeLabels {
    "Grade unit",
    "Grade (%)",
    "Grade (degrees)",
    "Grade rise",
    "Grade run",
    "Grade height (m)",
    "Grade plan run (m)",
    "Contour grade: 0 to 1000%; vertical is not supported. Height/run uses model metres.",
    true
};

bool RegionSlopeFields (Session& s, lg::Id object, const SlopeFieldLabels& labels, lg::TerrainSlope& slope,
                        bool& release)
{
    return SlopeFields (labels, slope, s.message,
                        [&] (const char* label, double& value, double lo, double hi, const char* format) {
                            return Slider (s, object, label, value, lo, hi, format, release);
                        });
}
} // namespace

// #112 P1: the panel's durable object is resolved from what is actually selected.
// A boundary-owner source line resolves its object directly (boundarySource is
// unique). An evaluated face resolves to the single candidate the compiled field
// plan lets win. #112 P5: the plan stacks entries by (root priority, topological
// position, object id) with later entries overriding earlier ones, so the panel
// ranks candidates by (priority, topological position) and takes the maximum --
// a derived result still follows its operands at equal priority, while an
// explicit priority overrides topological recency exactly as the build does.
// The plan's object-id tie-break only orders a shared-root stack deterministically;
// the panel keeps refusing that case as ambiguous rather than silently following
// object id -- the one intentional selection distinction. Anything else (no
// candidate, two objects sharing the winning root, disagreeing picks) refuses:
// the panel names no ambiguous domain.
edit::Id detail::SelectedRegionObject (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape || SelectedSourceRole (s) == lg::SourceRole::ReferenceLine)
        return edit::kNoId;
    const auto& evaluated = Landscape (s);
    std::vector<lg::Id> order;
    {
        std::string error;
        if (!lg::ValidateLandscapeDag (*doc.landscape, order, error))
            return edit::kNoId;
    }
    const auto rank = [&] (edit::Id objectId) {
        const auto& object = *std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                            [=] (const auto& o) { return o.id == objectId; });
        const auto root = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                        [=] (const auto& op) { return op.id == object.footprintOperation; });
        const auto it = std::find (order.begin (), order.end (), object.footprintOperation);
        return std::pair<int, int> (root != doc.landscape->operations.end () ? root->priority : 0,
                                    it == order.end () ? -1 : static_cast<int> (std::distance (order.begin (), it)));
    };
    std::set<edit::Id> resolved;
    for (const auto& ref : s.selection.Items ())
        if (ref.part == edit::Part::LandscapeFace && ref.revision == s.landscapeRevision) {
            const auto face = std::find_if (evaluated.faces.begin (), evaluated.faces.end (),
                                            [&] (const auto& f) { return f.faceId == ref.item; });
            if (face == evaluated.faces.end ())
                continue;
            std::set<edit::Id> candidates;
            for (const auto& region : evaluated.regions)
                if (region.objectId && FaceInside (face->seed, region.rings))
                    candidates.insert (region.objectId);
            if (candidates.empty ())
                continue;
            const auto winner = std::max_element (candidates.begin (), candidates.end (),
                                                  [&] (edit::Id a, edit::Id b) { return rank (a) < rank (b); });
            if (std::any_of (candidates.begin (), candidates.end (),
                             [&] (edit::Id c) { return c != *winner && rank (c) == rank (*winner); }))
                return edit::kNoId; // two objects share the winning root: ambiguous
            resolved.insert (*winner);
        }
        else if (ref.entity != edit::kNoId)
            for (const auto& object : doc.landscape->objects)
                if (object.boundarySource)
                    for (const auto& source : doc.landscape->sources)
                        if (!source.deleted && source.id == object.boundarySource && source.entityId == ref.entity)
                            resolved.insert (object.id);
    if (resolved.size () != 1)
        return edit::kNoId;
    s.landscapeObject = *resolved.begin ();
    return *resolved.begin ();
}

namespace {
std::optional<lg::HardscapeObject> ActiveRegion (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return std::nullopt;
    const auto selected = detail::SelectedRegionObject (s);
    if (selected == edit::kNoId)
        return std::nullopt;
    for (const auto& object : doc.landscape->objects)
        if (object.id == selected)
            return object;
    return std::nullopt;
}
} // namespace

bool CancelRegionSlider (Session& s)
{
    if (!s.regionSliderObject || s.regionSliderCancelled || !s.edits.InGesture ())
        return false;
    s.edits.Cancel ();
    s.regionSliderCancelled = true; // held until its release; the slider shows the restored value
    s.message.clear ();
    return true;
}

void RegionPanel (Session& s, bool materialsOnly)
{
    // A slider Esc gave up is forgotten once ImGui let go of it, released or no longer drawn.
    if (s.regionSliderCancelled && ImGui::GetActiveID () != s.regionSliderId)
        s.regionSliderObject = s.regionSliderId = 0, s.regionSliderCancelled = false;
    if (s.regionSliderObject && !s.regionSliderCancelled && !s.edits.InGesture ())
        s.regionSliderObject = s.regionSliderId = 0;
    const auto object = ActiveRegion (s);
    if (s.regionSliderObject && (!object || object->id != s.regionSliderObject || materialsOnly)) {
        if (!s.regionSliderCancelled)
            s.edits.Cancel ();
        s.regionSliderObject = s.regionSliderId = 0;
        s.regionSliderCancelled = false;
    }
    if (!object) {
        if (materialsOnly)
            ImGui::TextWrapped ("Select one closed Region boundary to edit its material metadata.");
        return;
    }
    ImGui::PushID (materialsOnly ? "region-material" : "region-surface");
    ImGui::PushID (std::to_string (object->id).c_str ());
    ImGui::Separator ();
    ImGui::Text ("Region %llu", static_cast<unsigned long long> (object->id));
    auto surface = object->surface.value_or (lg::RegionSurface {});
    auto material = object->baseMaterial;
    bool changed = false;
    auto terrain = object->terrain;
    bool terrainChanged = false;
    bool release = false;
    ImGui::BeginDisabled (GizmoModeLocked (s) || !s.drawing.empty () ||
                          (s.edits.InGesture () && !s.regionSliderObject));
    if (!materialsOnly) {
        int elevation = int (surface.elevation);
        if (ImGui::Combo ("Elevation", &elevation,
                          "Existing (no target)\0Constant (absolute metres)\0Boundary curve Z\0")) {
            surface.elevation = lg::RegionElevationMode (elevation);
            if (surface.elevation != lg::RegionElevationMode::Constant)
                surface.slope = lg::RegionSlopeMode::None;
            if (surface.elevation == lg::RegionElevationMode::Existing) {
                terrain.effect = lg::RegionTerrainEffect::None;
                terrainChanged = true;
            }
            changed = true;
        }
        ImGui::BeginDisabled (surface.elevation != lg::RegionElevationMode::Constant);
        int slope = int (surface.slope);
        if (ImGui::Combo ("Slope", &slope, "None (flat)\0Two points with elevations\0Contour line + grade\0")) {
            surface.slope = lg::RegionSlopeMode (slope);
            changed = true;
        }
        if (surface.slope == lg::RegionSlopeMode::None)
            changed |= Slider (s, object->id, "Elevation (m)", surface.first.z, surface.first.z - 10,
                               surface.first.z + 10, "%.3f", release);
        else {
            changed |= PointFields (s, object->id, "First point / contour origin", surface.first, true, release);
            changed |= PointFields (s, object->id, "Second point", surface.second,
                                    surface.slope == lg::RegionSlopeMode::TwoPoints, release);
            if (surface.slope == lg::RegionSlopeMode::ContourLine) {
                changed |= RegionSlopeFields (s, object->id, s_contourSlopeLabels, surface.grade, release);
                int side = int (surface.fallsTo);
                if (ImGui::Combo ("Fall side", &side, "Left\0Right\0")) {
                    surface.fallsTo = lg::RegionFallSide (side);
                    changed = true;
                }
                ImGui::TextWrapped (
                    "The contour runs first to second point. Fall is perpendicular, on the chosen side. "
                    "First point Z is the contour elevation; grade uses the selected unit.");
            }
            else {
                const double length = edit::DistanceXY (surface.first, surface.second);
                if (length > 1e-6)
                    ImGui::Text ("First to second: %+.2f %%", 100 * (surface.second.z - surface.first.z) / length);
            }
            edit::Vec3 first, second;
            const bool points = SelectedSlopePoints (s, first, second);
            ImGui::BeginDisabled (!points);
            if (ImGui::Button ("Use two selected vertices")) {
                surface.first = first;
                surface.second = second;
                changed = true;
            }
            ImGui::EndDisabled ();
            if (!points)
                ImGui::TextDisabled ("Select exactly two vertices, or enter their coordinates above.");
        }
        ImGui::EndDisabled ();
        if (surface.elevation == lg::RegionElevationMode::Boundary)
            ImGui::TextWrapped (
                "Boundary curve Z: the actual curve and vertex elevations define the triangulated surface. "
                "Move boundary points vertically to change the topography; Constant is an explicit override.");
        if (surface.elevation == lg::RegionElevationMode::Existing)
            ImGui::TextWrapped ("Existing has no vertical target: the captured terrain is unchanged. "
                                "Choose Constant to author a flat level or slope.");
        ImGui::BeginDisabled (surface.elevation == lg::RegionElevationMode::Existing);
        int effect = int (terrain.effect);
        // #112 P5: presentation names only — the persisted RegionTerrainEffect
        // values stay Grade/Add/Subtract; Add/Subtract are raise-only/lower-only
        // clamps, never signed displacement.
        if (ImGui::Combo ("Terrain interaction", &effect,
                          "None (target preview only)\0Grade (cut and fill)\0Raise-only\0Lower-only\0")) {
            terrain.effect = lg::RegionTerrainEffect (effect);
            terrainChanged = true;
        }
        int transition = int (terrain.transition);
        if (ImGui::Combo ("Ground transition", &transition, "Falloff\0Daylight slopes\0")) {
            terrain.transition = lg::RegionTerrainTransition (transition);
            terrainChanged = true;
        }
        if (terrain.transition == lg::RegionTerrainTransition::Falloff)
            terrainChanged |= Slider (s, object->id, "Falloff width (m)", terrain.falloff, 0.05, 100, "%.2f", release);
        else {
            terrainChanged |= Slider (s, object->id, "Maximum reach (m)", terrain.reach, 0.05, 100, "%.2f", release);
            terrainChanged |= RegionSlopeFields (s, object->id, s_cutSlopeLabels, terrain.cut, release);
            terrainChanged |= RegionSlopeFields (s, object->id, s_fillSlopeLabels, terrain.fill, release);
        }
        ImGui::EndDisabled ();
        ImGui::TextWrapped ("Union/Subtract above change the footprint, not vertical cut/fill. "
                            "Region targets grade first; path profiles take priority where composition succeeds. "
                            "Some mixed region/path scenes currently refuse (#82); no partial topography is published. "
                            "None is target preview only. Existing has no vertical target. Set a terrain to build the "
                            "actual topography.");
        if (TerrainGuid (s).empty ())
            ImGui::TextDisabled ("Set the topography in the terrain controls above.");
    }
    else {
        int preset = int (material.preset);
        if (ImGui::Combo ("Material preset metadata", &preset,
                          "None\0Hard paving\0Rubber playground\0Gravel\0Asphalt\0Reinforced grass\0Grass\0")) {
            material.preset = lg::MaterialPreset (preset);
            changed = true;
        }
        const auto fill = material.fillName.empty () ? material.fillGuid : material.fillName;
        const auto finish = material.surfaceName.empty () ? material.surfaceGuid : material.surfaceName;
        ImGui::TextWrapped ("Fill: %s", fill.empty () ? "not assigned" : fill.c_str ());
        ImGui::TextWrapped ("Surface: %s", finish.empty () ? "not assigned" : finish.c_str ());
        if (ImGui::Button ("Choose Archicad fill / surface..."))
            RequestMaterialPicker (s, object->id);
        if (ImGui::Button ("Refresh project materials")) {
            s.materialCatalogRevision = UINT64_MAX;
            s.materialCatalog.reset ();
        }
        if (s.materialCatalog && s.materialCatalogRevision == s.edits.Doc ().revision) {
            std::string error;
            if (!lg::ValidateMaterialAvailability (material, *s.materialCatalog, error))
                ImGui::TextWrapped ("%s", error.c_str ());
            else
                ImGui::TextDisabled ("Assigned native attributes resolve by GUID in this project.");
        }
        else if (!s.materialCatalogError.empty ())
            ImGui::TextWrapped ("%s", s.materialCatalogError.c_str ());
        else
            ImGui::TextDisabled ("Waiting for the project's fill/surface catalog.");
        ImGui::TextWrapped ("Shared material picker (#26): GUID/name persist; project indices do not. "
                            "Missing materials need explicit replacement or clearing, never a preset substitution. "
                            "Native hatch-pattern rendering and output acceptance remain separate.");
    }
    if ((changed || terrainChanged) && !s.regionSliderCancelled) {
        const auto operation = [id = object->id, surface, material, terrain] (edit::Document& doc, std::string& error) {
            if (surface.elevation == lg::RegionElevationMode::Existing &&
                !lg::SetRegionTerrain (doc, id, terrain, error))
                return false;
            return lg::SetRegionSurface (doc, id, surface, material, error) &&
                   lg::SetRegionTerrain (doc, id, terrain, error);
        };
        if (!(s.regionSliderObject ? s.edits.Move (operation) : s.edits.Apply (operation)))
            s.message = s.edits.Message ();
    }
    if (release && s.regionSliderObject) {
        if (!s.regionSliderCancelled)
            s.edits.End ();
        s.regionSliderObject = s.regionSliderId = 0;
        s.regionSliderCancelled = false;
    }
    ImGui::EndDisabled ();
    ImGui::PopID ();
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::edithud
