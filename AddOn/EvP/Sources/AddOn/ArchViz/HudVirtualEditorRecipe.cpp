// The Operations tab's terrain recipe (#105 slices 1-2, #157 G2/G3). It shows what the
// terrain build will actually do -- route, effective order, targets, footprint construction,
// constraints, owners left out and their reasons, limitations -- from the one compiled plan
// (Edit/EditLandscapeRecipe.hpp). A row click only selects the row's source in the view; the
// row's own controls (slice 2) are the supported edits, each one undo step. Step 4: the published
// terrain's quantity rows, named after the recipe rows, with Build and Copy.
// OVERLAY-INVARIANTS.md §8: its cache lives on the editor Session.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/HudVirtualEditorRecipe.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditTerrainDesignJob.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

namespace {
constexpr ImVec4 kNote { 0.45f, 0.45f, 0.50f, 1.0f };
constexpr ImVec4 kLimit { 0.80f, 0.45f, 0.05f, 1.0f };

// The view's counterpart of a row: the region's boundary line or the designed path. #189: a
// Boolean result has no line of its own, so its counterpart is its construction inputs' lines.
std::vector<edit::Id> RowEntities (const Session& s, const lg::RecipeRow& row)
{
    if (row.path != edit::kNoId)
        return { row.path };
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return {};
    const auto line = [&] (edit::Id objectId) {
        for (const auto& object : doc.landscape->objects)
            if (object.id == objectId && object.boundarySource != edit::kNoId)
                for (const auto& source : doc.landscape->sources)
                    if (source.id == object.boundarySource && !source.deleted)
                        return source.entityId;
        return edit::kNoId;
    };
    if (const auto own = line (row.object))
        return { own };
    std::vector<edit::Id> lines;
    if (row.object)
        for (const auto& input : lg::LandscapeConstructionInputs (*doc.landscape))
            if (input.owner == row.object)
                if (const auto entity = line (input.object))
                    lines.push_back (entity);
    return lines;
}
bool RowSelected (const Session& s, const std::vector<edit::Id>& entities)
{
    return !entities.empty () && std::all_of (entities.begin (), entities.end (), [&] (edit::Id entity) {
        return s.selection.Contains ({ entity, edit::Part::Entity, 0 });
    });
}
void SelectRow (Session& s, const std::vector<edit::Id>& entities)
{
    std::vector<edit::Ref> refs;
    for (const auto entity : entities)
        refs.push_back ({ entity, edit::Part::Entity, 0 });
    s.selection.Set (refs);
}

// #105 slice 2: the recipe's supported edits, each one undo step. Order moves only regions within
// the region stage; "Footprint on" bypasses the root step; Effect is the terrain participation.
// A refused edit changes nothing and says why.
void Controls (Session& s, const lg::RecipeRow& row)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape || !row.object)
        return;
    const auto object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                      [&] (const lg::HardscapeObject& o) { return o.id == row.object; });
    if (object == doc.landscape->objects.end () || object->pathId)
        return;
    const edit::Id id = object->id;
    const auto terrain = object->terrain;
    const edit::Id rootId = object->footprintOperation;
    const auto root = std::find_if (doc.landscape->operations.begin (), doc.landscape->operations.end (),
                                    [&] (const lg::LandscapeOperation& o) { return o.id == rootId; });
    const auto apply = [&] (auto&& edit) {
        if (!s.edits.Apply (edit))
            s.message = s.edits.Message ();
    };
    if (row.stage == lg::RecipeStage::Region) {
        const auto move = [&] (int step) {
            apply ([id, step] (edit::Document& d, std::string& error) {
                return lg::MoveRecipeRegion (d, id, step, error);
            });
        };
        // #191 U5: an arrow that would be refused is disabled, and its tooltip says why.
        const auto arrow = [&] (const char* widget, ImGuiDir dir, int step, const char* tip) {
            const auto refusal = RecipeMoveRefusal (s, id, step);
            ImGui::BeginDisabled (!refusal.empty ());
            if (ImGui::ArrowButton (widget, dir))
                move (step);
            ImGui::EndDisabled ();
            if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
                geomsrv::archviz::hudshell::HoverTip ("%s", refusal.empty () ? tip : refusal.c_str ());
        };
        arrow ("##up", ImGuiDir_Up, -1, "Move up: compose earlier, under the rows below it.");
        ImGui::SameLine ();
        arrow ("##down", ImGuiDir_Down, 1,
               "Move down: compose later, over the rows above it. Paths always stay after regions.");
        ImGui::SameLine ();
    }
    // A plain region's root selects its one boundary: bypassing it would pass the same shape through,
    // so the switch is offered only where it changes the footprint.
    if (root != doc.landscape->operations.end () &&
        !(root->kind == lg::OperationKind::RegionSelect && root->faces.empty () && root->inputs.size () == 1)) {
        bool on = root->enabled;
        const auto refusal = RecipeBypassRefusal (s, rootId);
        ImGui::BeginDisabled (!refusal.empty ());
        if (ImGui::Checkbox ("Footprint on", &on))
            apply ([rootId, on] (edit::Document& d, std::string& error) {
                return lg::SetLandscapeOperationEnabled (d, rootId, on, error);
            });
        ImGui::EndDisabled ();
        if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
            geomsrv::archviz::hudshell::HoverTip (
                "%s", !refusal.empty () ? refusal.c_str ()
                                        : "Off bypasses the footprint's last step (its first input passes through). "
                                          "The terrain effect is a separate setting.");
        ImGui::SameLine ();
    }
    // #210: a Boolean result's row takes it apart.
    if (root != doc.landscape->operations.end () &&
        (root->kind == lg::OperationKind::Union || root->kind == lg::OperationKind::Difference ||
         root->kind == lg::OperationKind::Intersection || root->kind == lg::OperationKind::Xor)) {
        if (ImGui::SmallButton ("Dissolve"))
            DissolveBooleanResult (s, id);
        if (ImGui::IsItemHovered ())
            geomsrv::archviz::hudshell::HoverTip (
                "Dissolve the Boolean: its input shapes become regions of their own again, with the settings "
                "they kept. One undo step.");
        ImGui::SameLine ();
    }
    constexpr lg::RegionTerrainEffect effects[] = { lg::RegionTerrainEffect::None, lg::RegionTerrainEffect::Grade,
                                                    lg::RegionTerrainEffect::Add, lg::RegionTerrainEffect::Subtract };
    ImGui::SetNextItemWidth (130.0f);
    if (ImGui::BeginCombo ("Effect", lg::RecipeEffectName (terrain.effect))) {
        for (const auto effect : effects)
            if (ImGui::Selectable (lg::RecipeEffectName (effect), effect == terrain.effect) &&
                effect != terrain.effect) {
                auto changed = terrain;
                changed.effect = effect;
                apply ([id, changed] (edit::Document& d, std::string& error) {
                    return lg::SetRegionTerrain (d, id, changed, error);
                });
            }
        ImGui::EndCombo ();
    }
    if (ImGui::IsItemHovered ())
        geomsrv::archviz::hudshell::HoverTip (
            "None leaves the terrain recipe; the footprint stays. Grade, Raise only and Lower only "
            "need a target other than Existing.");
}

// #189: the regions a row's footprint consumed, as its children: dashed in the view, no effect or
// quantities of their own, each still selectable and editable.
void ConstructionInputs (Session& s, const lg::LandscapeRecipe& recipe, edit::Id consumer, int depth = 0)
{
    for (const auto& input : recipe.rows) {
        if (input.constructionOf != consumer || input.constructionOf == edit::kNoId || depth > 16)
            continue;
        ImGui::PushID (int (input.object));
        const auto entities = RowEntities (s, input);
        const std::string title = "Input:  " + input.name + " " + std::to_string (input.object);
        if (ImGui::Selectable (title.c_str (), RowSelected (s, entities)) && !entities.empty ())
            SelectRow (s, entities);
        if (ImGui::IsItemHovered ())
            geomsrv::archviz::hudshell::HoverTip (
                "A construction input: dashed in the view and still editable. It has no terrain effect or "
                "quantities while this region is built from it.");
        ImGui::Indent ();
        ConstructionInputs (s, recipe, input.object, depth + 1);
        ImGui::Unindent ();
        ImGui::PopID ();
    }
}

void Row (Session& s, const lg::LandscapeRecipe& recipe, const lg::RecipeRow& row)
{
    ImGui::PushID (row.object ? int (row.object) : int (row.operation ? row.operation : row.source));
    const auto entities = RowEntities (s, row);
    std::string title = row.order ? std::to_string (row.order) + ".  " : "-   ";
    title += row.name;
    if (row.kind != row.name)
        title += "  (" + row.kind + ")";
    if (ImGui::Selectable (title.c_str (), RowSelected (s, entities)) && !entities.empty ())
        SelectRow (s, entities);
    if (ImGui::IsItemHovered () && entities.empty ())
        geomsrv::archviz::hudshell::HoverTip (
            "This row has no single line in the view (a derived domain or a constraint).");
    ImGui::Indent ();
    if (!row.effect.empty ())
        ImGui::Text ("Effect: %s%s%s", row.effect.c_str (),
                     row.target.empty () ? "" : "   Target: ", row.target.c_str ());
    Controls (s, row);
    if (row.object)
        ConstructionInputs (s, recipe, row.object);
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
    // #189: a construction input is listed under the row built from it, not as a row of its own.
    const auto listed = [&] (const lg::RecipeRow& r) { return r.stage == stage && !r.constructionOf; };
    if (!std::any_of (recipe.rows.begin (), recipe.rows.end (), listed))
        return;
    ImGui::SeparatorText (heading);
    for (const auto& row : recipe.rows)
        if (listed (row))
            Row (s, recipe, row);
}
// #105 step 4: the published terrain's quantities, named after the recipe rows. Read-only: a row
// click selects its owner's line; Build asks for the current revision's terrain; Copy puts the
// table on the clipboard.
void Quantities (Session& s, const lg::LandscapeRecipe& recipe, const TerrainDesignStatus& status)
{
    ImGui::SeparatorText ("Quantities");
    bool stale = false;
    const auto* last = LastPublishedTerrainDesign (s, stale);
    const auto quantities = lg::ProjectRecipeQuantities (recipe, last ? last->published.get () : nullptr, stale);
    // #191 Decided 4: Build lives with nothing selected and on the state chip, not here.
    if (status.pending)
        ImGui::TextDisabled ("Building the terrain of this revision...");
    else if (recipe.ok && recipe.route != lg::RecipeRoute::Nothing && !quantities.current)
        ImGui::TextDisabled ("Not built from this revision: Build mesh, or click the state chip.");
    if (!quantities.available) {
        ImGui::TextWrapped ("%s", quantities.reason.c_str ());
        return;
    }
    if (ImGui::Button ("Copy table"))
        ImGui::SetClipboardText (lg::RecipeQuantitiesText (quantities).c_str ());
    if (ImGui::IsItemHovered ())
        geomsrv::archviz::hudshell::HoverTip ("Copy the rows and the total as tab-separated text.");
    if (!quantities.current) {
        ImGui::PushStyleColor (ImGuiCol_Text, kLimit);
        ImGui::TextWrapped ("Built from revision %llu, not this one.",
                            static_cast<unsigned long long> (quantities.revision));
        ImGui::PopStyleColor ();
    }
    // #191 U5: Area and Net shown by default; Cut and Fill from the header's right-click menu.
    // Numbers sit in fixed columns, right-aligned and never wrapped; one too wide for its column
    // is clipped and shown whole on hover.
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Hideable |
                                      ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable ("##quantities", 5, flags)) {
        const float number = ImGui::CalcTextSize ("-00000.000").x;
        ImGui::TableSetupColumn ("Owner", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
        ImGui::TableSetupColumn ("Area m2", ImGuiTableColumnFlags_WidthFixed, number);
        ImGui::TableSetupColumn ("Net m3", ImGuiTableColumnFlags_WidthFixed, number);
        ImGui::TableSetupColumn ("Cut m3", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
                                 number);
        ImGui::TableSetupColumn ("Fill m3", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide,
                                 number);
        ImGui::TableHeadersRow ();
        const auto numbers = [] (double cut, double fill, double net, double area) {
            for (const double value : { area, net, cut, fill }) {
                if (!ImGui::TableNextColumn ())
                    continue;
                char text[32];
                std::snprintf (text, sizeof text, "%.3f", value);
                const float width = ImGui::CalcTextSize (text).x, room = ImGui::GetContentRegionAvail ().x;
                if (width < room)
                    ImGui::SetCursorPosX (ImGui::GetCursorPosX () + room - width);
                ImGui::TextUnformatted (text);
                if (width > room && ImGui::IsItemHovered ())
                    geomsrv::archviz::hudshell::HoverTip ("%s", text);
            }
        };
        for (const auto& row : quantities.rows) {
            ImGui::TableNextRow ();
            ImGui::TableNextColumn ();
            ImGui::PushID (row.baseline ? -1 : int (row.order));
            const auto owner = std::find_if (recipe.rows.begin (), recipe.rows.end (), [&] (const lg::RecipeRow& r) {
                return !row.baseline && r.order != 0 && r.object == row.object && r.operation == row.operation;
            });
            const auto entities = owner != recipe.rows.end () ? RowEntities (s, *owner) : std::vector<edit::Id> {};
            const std::string label = row.baseline ? row.name : std::to_string (row.order) + ".  " + row.name;
            if (ImGui::Selectable (label.c_str (), RowSelected (s, entities), ImGuiSelectableFlags_SpanAllColumns) &&
                !entities.empty ())
                SelectRow (s, entities);
            ImGui::PopID ();
            numbers (row.cut, row.fill, row.net, row.area);
        }
        ImGui::TableNextRow ();
        ImGui::TableNextColumn ();
        ImGui::TextUnformatted ("Total");
        numbers (quantities.total.cut, quantities.total.fill, quantities.total.net, quantities.total.area);
        ImGui::EndTable ();
    }
    ImGui::TextDisabled ("Property, 0.25 m grid: cut %.3f m3, fill %.3f m3. Outside it: cut %.3f m3, fill %.3f m3.",
                         quantities.site.cut, quantities.site.fill, quantities.context.cut, quantities.context.fill);
    for (const auto& note : quantities.notes) {
        ImGui::PushStyleColor (ImGuiCol_Text, kNote);
        ImGui::TextWrapped ("%s", note.c_str ());
        ImGui::PopStyleColor ();
    }
}
} // namespace

std::string RecipeBypassRefusal (const Session& s, lg::Id operation)
{
    const auto& doc = s.edits.Doc ();
    if (!doc.landscape)
        return {};
    const auto& session = *doc.landscape;
    const auto op = std::find_if (session.operations.begin (), session.operations.end (),
                                  [&] (const lg::LandscapeOperation& o) { return o.id == operation; });
    if (op == session.operations.end () || !op->enabled)
        return {}; // turning a step back on is always allowed
    const bool root = std::any_of (session.objects.begin (), session.objects.end (),
                                   [&] (const lg::HardscapeObject& o) { return o.footprintOperation == operation; });
    if (!root || lg::FootprintBypassBuildable (session, operation))
        return {};
    return "This step reads its line directly: bypassed, it would leave no step the terrain build can grade "
           "from. Set Effect to None to take the region out of the terrain instead.";
}

std::string RecipeMoveRefusal (Session& s, lg::Id object, int step)
{
    const auto& doc = s.edits.Doc ();
    if (s.recipeMovesRevision != doc.revision)
        s.recipeMoves.clear (), s.recipeMovesRevision = doc.revision;
    for (const auto& [key, reason] : s.recipeMoves)
        if (key.first == object && key.second == step)
            return reason;
    auto copy = doc;
    std::string error;
    if (lg::MoveRecipeRegion (copy, object, step, error))
        error.clear ();
    else if (error.empty ())
        error = "This row cannot move that way.";
    s.recipeMoves.push_back ({ { object, step }, error });
    return error;
}

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
    CurrentRecipe (s);
    // Held for the whole frame: a row's edit publishes a revision, and the next CurrentRecipe call
    // replaces the cache this loop is still reading.
    const auto keep = s.recipe;
    const auto& recipe = *keep;
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
    Quantities (s, recipe, status);
}
} // namespace geomsrv::archviz::edithud
