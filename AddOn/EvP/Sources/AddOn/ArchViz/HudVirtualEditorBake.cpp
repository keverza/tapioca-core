#include "ArchViz/HudVirtualEditorBake.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "ArchViz/HudVirtualEditorTerrainState.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "Edit/EditPathContours.hpp"
#include "Edit/EditPublishedTerrain.hpp"
#include "Edit/EditTerrainBake.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// tapioca-dev #177. Session-owned derived geometry and a queued request only: no model read or
// write here, none from ImGui layout.
namespace geomsrv::archviz::edithud {
namespace {
#if defined(TAPIOCA_GEOMETRY_RUST)
// The built surface that follows the document, with its generation; null otherwise.
const tapioca::geometry::TerrainDesignOutcome* Built (const Session& s, std::string* why)
{
    const auto say = [&] (const char* text) {
        if (why)
            *why = text;
        return nullptr;
    };
    if (!s.terrainLive || !s.terrainDesign)
        return say ("Set a terrain and build its mesh first.");
    bool stale = false;
    const auto* last = LastPublishedTerrainDesign (s, stale);
    if (!last || !last->built)
        return say ("Build the terrain mesh first: the Archicad Mesh is made from the built surface.");
    if (stale || GetTerrainDesignStatus (s).pending)
        return say ("The built mesh does not follow the last edits. Build it again first.");
    return last;
}
#endif

// A real, always: a whole number keeps its ".0".
void Real (std::string& out, double value)
{
    char text[40];
    std::snprintf (text, sizeof text, "%.12g", value);
    out += text;
    if (!std::strpbrk (text, ".eEn"))
        out += ".0";
}
void Reals (std::string& out, const char* name, const std::vector<double>& values)
{
    out += "\"";
    out += name;
    out += "\":[";
    for (size_t i = 0; i < values.size (); ++i) {
        if (i)
            out += ",";
        Real (out, values[i]);
    }
    out += "]";
}
} // namespace

const edit::BakePlan* TerrainBakePlan (Session& s, std::string* why)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    const auto* built = Built (s, why);
    if (!built)
        return nullptr;
    auto& bake = s.bake;
    if (bake.generation != built->generation || !(bake.planned == bake.options)) {
        const auto* published = built->published.get ();
        bake.plan =
            edit::PlanTerrainBake (published ? published->points : built->result.build.points,
                                   published ? published->triangles : built->result.build.triangles, bake.options);
        bake.generation = built->generation;
        bake.planned = bake.options;
    }
    if (!bake.plan.ok && why)
        *why = bake.plan.error;
    return &bake.plan;
#else
    (void) s;
    if (why)
        *why = "Terrain design requires the Rust geometry build.";
    return nullptr;
#endif
}

void DrawTerrainBake (const Session& s, overlaylayers::Layer& layer)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    const auto& bake = s.bake;
    if (!bake.preview || !bake.plan.ok)
        return;
    const auto* built = Built (s, nullptr);
    if (!built || built->generation != bake.generation)
        return;
    overlaylayers::PointSet marks;
    marks.rgba = 0x111827FFu;
    marks.sizePixels = 5.0f;
    marks.sizeMetres = 0.08f;
    const auto add = [&] (const std::vector<edit::Vec3>& points, bool closed, uint32_t rgba, float width) {
        overlaylayers::Polyline line;
        for (const auto& p : points) {
            line.points.insert (line.points.end (), { p.x, p.y, p.z });
            marks.points.insert (marks.points.end (), { p.x, p.y, p.z });
        }
        line.closed = closed;
        line.rgba = rgba;
        line.widthPixels = width;
        line.behind = overlaylayers::Behind::Show;
        layer.polylines.push_back (std::move (line));
    };
    // The Mesh's outline, its contours and its creases, each in its own colour; a mark at
    // every point the Mesh would get.
    add (bake.plan.outline, true, 0xEA580CFFu, 2.5f);
    for (const auto& line : bake.plan.lines)
        add (line.points, line.closed, line.kind == edit::BakeLineKind::Crease ? 0x1D4ED8FFu : 0xC026D3FFu, 2.0f);
    layer.points.push_back (std::move (marks));
#else
    (void) s;
    (void) layer;
#endif
}

std::string TerrainBakeRequestJson (const Session& s)
{
    const auto payload = edit::TerrainBakePayload (s.bake.plan, s.bake.skirt);
    if (payload.outline.empty () || !edit::CheckBakePayload (payload).empty ())
        return {};
    std::string out = "{";
    Reals (out, "outline", payload.outline);
    out += ",";
    Reals (out, "polyZ", payload.polyZ);
    out += ",\"baseLevel\":";
    Real (out, payload.baseLevel);
    // The heights are the editor's, which are the model's: the command places the base plane
    // on the story it falls in.
    out += ",\"absoluteBase\":true,\"skirtLevel\":0.0,\"skirt\":\"SolidBodyWithSkirt\",\"onFloorPlan\":true";
    if (!payload.ridgeCounts.empty ()) {
        out += ",";
        Reals (out, "ridgeCoords", payload.ridgeCoords);
        out += ",\"ridgeCounts\":[";
        for (size_t i = 0; i < payload.ridgeCounts.size (); ++i)
            out += (i ? "," : "") + std::to_string (payload.ridgeCounts[i]);
        out += "]";
    }
    return out + "}";
}

bool RequestTerrainBake (Session& s)
{
    if (s.bake.requested)
        return false;
    std::string why;
    const auto* plan = TerrainBakePlan (s, &why);
    if (!plan || !plan->ok) {
        s.message = why.empty () ? "There is no terrain to make a Mesh from." : why;
        return false;
    }
    if (s.edits.InGesture () || !s.drawing.empty () || s.landscapeCommand || s.terrainCapture || s.draftingImport) {
        s.message = "Finish the current input before creating the Mesh.";
        return false;
    }
    const auto problems = edit::CheckBakePayload (edit::TerrainBakePayload (*plan, s.bake.skirt));
    if (!problems.empty ()) {
        s.message = "The Mesh was not created: " + problems.front ();
        return false;
    }
    s.bake.requested = true;
    s.bake.status = "Creating the Archicad Mesh...";
    return true;
}

void FinishTerrainBake (Session& s, bool created, const std::string& message)
{
    s.bake.requested = s.bake.queued = false;
    s.bake.status = message;
    s.message = message;
    if (created)
        s.bake.preview = false; // the Mesh itself is in the model now
}

bool RevealTerrainBake (Session& s)
{
    if (!s.terrainLive || !s.terrainDesign) {
        s.message = "Set a terrain and turn on Rebuild terrain (3D) first: the Archicad Mesh is made from the built "
                    "terrain.";
        return false;
    }
    if (s.edits.InGesture () || !s.drawing.empty () || s.landscapeCommand) {
        s.message = "Finish the current input first.";
        return false;
    }
    // The section is the terrain's, shown with nothing selected.
    s.selection.Clear ();
    s.bake.reveal = true;
    return true;
}

void TerrainBakePanel (Session& s)
{
    const bool reveal = s.bake.reveal;
    if (reveal)
        ImGui::SetNextItemOpen (true);
    const bool open = ImGui::CollapsingHeader ("Archicad Mesh");
    if (reveal) {
        ImGui::SetScrollHereY (0.0f);
        s.bake.reveal = false;
    }
    if (!open)
        return;
    auto& bake = s.bake;
    auto& options = bake.options;
    ImGui::BeginDisabled (bake.requested);
    int mode = int (options.mode);
    ImGui::RadioButton ("Contour lines", &mode, int (edit::BakeMode::Contours));
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("Horizontal cuts of the built surface at one interval.");
    ImGui::SameLine ();
    ImGui::RadioButton ("Signature lines", &mode, int (edit::BakeMode::Signature));
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("The surface's own edges - pads, toes, kerbs, the terrain's ridges - with a few "
                            "contours for the soft ground between them.");
    options.mode = edit::BakeMode (mode);
    const bool signature = options.mode == edit::BakeMode::Signature;
    // Labels above their controls: the panel is narrow.
    const float width = std::min (ImGui::GetContentRegionAvail ().x, 220.0f);
    float interval = float (options.interval);
    ImGui::TextWrapped ("%s", signature ? "Soft-ground contour interval (m, 0: automatic)"
                                        : "Contour interval (m, 0: automatic)");
    ImGui::SetNextItemWidth (width);
    if (ImGui::DragFloat ("##bake-interval", &interval, 0.05f, 0.0f, 10.0f, "%.2f"))
        options.interval = double (std::clamp (interval, 0.0f, 10.0f));
    float tolerance = float (options.tolerance);
    ImGui::TextWrapped ("%s", "Point reduction (m)");
    ImGui::SetNextItemWidth (width);
    if (ImGui::SliderFloat ("##bake-tolerance", &tolerance, 0.0f, 0.5f, "%.3f"))
        options.tolerance = double (std::clamp (tolerance, 0.0f, 0.5f));
    if (ImGui::IsItemHovered ())
        hudshell::HoverTip ("A point is dropped while its line stays within this distance of the traced one.");
    if (signature) {
        float crease = float (options.creaseDegrees);
        ImGui::TextWrapped ("%s", "Edge angle (degrees)");
        ImGui::SetNextItemWidth (width);
        if (ImGui::SliderFloat ("##bake-crease", &crease, 0.5f, 30.0f, "%.1f"))
            options.creaseDegrees = double (std::clamp (crease, 0.5f, 30.0f));
        if (ImGui::IsItemHovered ())
            hudshell::HoverTip ("An edge is a line where its two faces meet at more than this angle.");
    }
    float skirt = float (bake.skirt);
    ImGui::TextWrapped ("%s", "Depth under the lowest point (m)");
    ImGui::SetNextItemWidth (width);
    if (ImGui::DragFloat ("##bake-skirt", &skirt, 0.05f, 0.0f, 50.0f, "%.2f"))
        bake.skirt = double (std::clamp (skirt, 0.0f, 50.0f));
    ImGui::Checkbox ("Show the lines and points", &bake.preview);
    ImGui::EndDisabled ();

    std::string why;
    const auto* plan = TerrainBakePlan (s, &why);
    if (!plan || !plan->ok) {
        ImGui::TextWrapped ("%s", why.c_str ());
        if (!bake.status.empty ())
            ImGui::TextWrapped ("%s", bake.status.c_str ());
        return;
    }
    size_t creases = 0, contours = 0;
    for (const auto& line : plan->lines)
        (line.kind == edit::BakeLineKind::Crease ? creases : contours) += 1;
    ImGui::TextWrapped ("Outline: %zu points (%zu traced)", plan->outline.size (), plan->outlineTraced);
    if (signature)
        ImGui::TextWrapped ("%zu edge lines, %zu contours at %.*f m", creases, contours,
                            edit::StepDecimals (plan->interval), plan->interval);
    else
        ImGui::TextWrapped ("%zu contours at %.*f m", contours, edit::StepDecimals (plan->interval), plan->interval);
    ImGui::TextWrapped ("Line points: %zu (%zu traced)", plan->points, plan->traced);
    for (const auto& note : plan->notes)
        ImGui::TextWrapped ("%s", note.c_str ());
    ImGui::BeginDisabled (bake.requested);
    if (ImGui::Button (bake.requested ? "Creating..." : "Create Archicad Mesh"))
        RequestTerrainBake (s);
    ImGui::EndDisabled ();
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        hudshell::HoverTip ("Creates one Mesh in the model from the lines shown. One Undo in Archicad removes it. "
                            "The terrain it was drawn on is left as it is.");
    if (!bake.status.empty ())
        ImGui::TextWrapped ("%s", bake.status.c_str ());
}
} // namespace geomsrv::archviz::edithud
