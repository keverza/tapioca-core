// The Operations tab's terrain recipe (#105 slice 1, #157 G2): read-only. It shows what the
// terrain build will actually do -- route, effective order, targets, footprint construction,
// constraints, owners left out and their reasons, limitations -- from the one compiled plan
// (Edit/EditLandscapeRecipe.hpp). It edits nothing: a row click only selects the row's source
// in the view. OVERLAY-INVARIANTS.md §8: its cache lives on the editor Session.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorRecipe.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include <imgui.h>
#include <algorithm>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

namespace {
constexpr ImVec4 kNote { 0.45f, 0.45f, 0.50f, 1.0f };
constexpr ImVec4 kLimit { 0.80f, 0.45f, 0.05f, 1.0f };

// The view's counterpart of a row: the region's boundary line or the designed path.
edit::Id RowEntity (const Session& s, const lg::RecipeRow& row)
{
    if (row.path != edit::kNoId)
        return row.path;
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return edit::kNoId;
    for (const auto& object : doc.landscape->objects)
        if (object.id == row.object && object.boundarySource != edit::kNoId)
            for (const auto& source : doc.landscape->sources)
                if (source.id == object.boundarySource && !source.deleted)
                    return source.entityId;
    return edit::kNoId;
}

void Row (Session& s, const lg::RecipeRow& row)
{
    ImGui::PushID (row.object ? int (row.object) : int (row.operation ? row.operation : row.source));
    const edit::Id entity = RowEntity (s, row);
    const bool selected = entity != edit::kNoId && s.selection.Contains ({ entity, edit::Part::Entity, 0 });
    std::string title = row.order ? std::to_string (row.order) + ".  " : "-   ";
    title += row.name;
    if (row.kind != row.name)
        title += "  (" + row.kind + ")";
    if (ImGui::Selectable (title.c_str (), selected) && entity != edit::kNoId)
        s.selection.Set ({ { entity, edit::Part::Entity, 0 } });
    if (ImGui::IsItemHovered () && entity == edit::kNoId)
        ImGui::SetTooltip ("This row has no single line in the view (a derived domain or a constraint).");
    ImGui::Indent ();
    if (!row.effect.empty ())
        ImGui::Text ("Effect: %s%s%s", row.effect.c_str (),
                     row.target.empty () ? "" : "   Target: ", row.target.c_str ());
    if (row.stage == lg::RecipeStage::Region && row.priority != 0)
        ImGui::Text ("Priority %d", row.priority);
    for (const auto& note : row.notes) {
        ImGui::PushStyleColor (ImGuiCol_Text, kNote);
        ImGui::TextWrapped ("%s", note.c_str ());
        ImGui::PopStyleColor ();
    }
    if (!row.construction.empty () && ImGui::TreeNode ("Footprint")) {
        for (const auto& node : row.construction) {
            ImGui::Indent (float (node.depth) * 12.0f + 1.0f);
            ImGui::Text ("%s%s%s", node.label.c_str (), node.bypassed ? "   bypassed" : "",
                         node.reference ? "   (shared, shown above)" : "");
            ImGui::Unindent (float (node.depth) * 12.0f + 1.0f);
        }
        ImGui::TreePop ();
    }
    ImGui::Unindent ();
    ImGui::PopID ();
}

void Section (Session& s, const lg::LandscapeRecipe& recipe, lg::RecipeStage stage, const char* heading)
{
    const bool any = std::any_of (recipe.rows.begin (), recipe.rows.end (),
                                  [&] (const lg::RecipeRow& r) { return r.stage == stage; });
    if (!any)
        return;
    ImGui::SeparatorText (heading);
    for (const auto& row : recipe.rows)
        if (row.stage == stage)
            Row (s, row);
}
} // namespace

const lg::LandscapeRecipe& CurrentRecipe (Session& s)
{
    const auto request = TerrainRecipeRequest (s);
    if (!s.recipe || s.recipeRevision != s.edits.Doc ().revision || s.recipeRequest != request) {
        s.recipe = std::make_shared<const lg::LandscapeRecipe> (lg::ProjectLandscapeRecipe (s.edits.Doc (), request));
        s.recipeRevision = s.edits.Doc ().revision;
        s.recipeRequest = request;
    }
    return *s.recipe;
}

void RecipePanel (Session& s)
{
    const auto& recipe = CurrentRecipe (s);
    ImGui::TextWrapped ("%s", recipe.routeText.c_str ());
    if (!recipe.ok) {
        ImGui::PushStyleColor (ImGuiCol_Text, kLimit);
        ImGui::TextWrapped ("No recipe: %s", recipe.error.c_str ());
        ImGui::PopStyleColor ();
    }
    // Which revision this describes, and whether the terrain shown is built from it.
    const auto status = GetTerrainDesignStatus (s);
    if (recipe.hash)
        ImGui::TextDisabled ("Revision %llu   plan %016llx", static_cast<unsigned long long> (recipe.revision),
                             static_cast<unsigned long long> (recipe.hash));
    else
        ImGui::TextDisabled ("Revision %llu", static_cast<unsigned long long> (recipe.revision));
    if (status.active) {
        ImGui::SameLine ();
        if (status.pending)
            ImGui::TextDisabled ("   terrain: building");
        else if (status.stale)
            ImGui::TextDisabled ("   terrain shown: revision %llu (stale)",
                                 static_cast<unsigned long long> (status.revision));
        else if (status.current && status.built)
            ImGui::TextDisabled ("   terrain: built from this revision");
        else if (!status.error.empty ())
            ImGui::TextDisabled ("   terrain: refused");
    }
    for (const auto& limitation : recipe.limitations) {
        ImGui::PushStyleColor (ImGuiCol_Text, kLimit);
        ImGui::TextWrapped ("%s", limitation.c_str ());
        ImGui::PopStyleColor ();
    }
    Section (s, recipe, lg::RecipeStage::Region, "Terrain stack: regions, later over earlier");
    Section (s, recipe, lg::RecipeStage::Path, "Then the path");
    Section (s, recipe, lg::RecipeStage::Mask, "Constraints: masks");
    Section (s, recipe, lg::RecipeStage::Control, "Constraints: fixed heights");
    Section (s, recipe, lg::RecipeStage::Excluded, "Not in the terrain recipe");
    if (!recipe.diagnostics.empty ()) {
        ImGui::SeparatorText ("Diagnostics");
        for (const auto& d : recipe.diagnostics)
            ImGui::TextWrapped ("%s", d.c_str ());
    }
}
} // namespace geomsrv::archviz::edithud
