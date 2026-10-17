// OVERLAY-INVARIANTS.md: pure layer content/state, no renderer or camera changes.
#include "SurfaceTakeoffLayer.hpp"
#include <iomanip>
#include <sstream>

namespace geomsrv::archviz::surfacetakeoff {
namespace {
namespace layers = overlaylayers;
std::string Area (double value)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision (3) << value;
    return text.str ();
}
bool Included (Facing facing, const UiState& state)
{
    return facing == Facing::Top      ? state.includeTop
           : facing == Facing::Bottom ? state.includeBottom
                                      : state.includeSides;
}
} // namespace
UiState InitialUi (const Presentation& presentation, const std::string& token)
{
    UiState state;
    state.token = token;
    for (const auto& row : presentation.rows)
        state.selected.insert (row.index);
    state.pending = state.selected;
    return state;
}
std::string UiId (const UiState& state, const char* action, int32_t surface)
{
    return state.token + ":" + action + ":" + std::to_string (surface);
}
bool ApplyUiChange (const Presentation& presentation, UiState& state, const std::string& kind, const std::string& id,
                    double value, const std::string& text)
{
    if (ApplyCriteriaChange (presentation, state, kind, id, value, text))
        return true;
    if (kind == "checkbox" && (value == 0 || value == 1)) {
        bool* flag = nullptr;
        if (id == UiId (state, "top"))
            flag = &state.includeTop;
        else if (id == UiId (state, "bottom"))
            flag = &state.includeBottom;
        else if (id == UiId (state, "sides"))
            flag = &state.includeSides;
        if (flag) {
            *flag = value == 1;
            return true;
        }
    }
    if (kind == "button" && id == UiId (state, "calculate")) {
        state.selected = state.pending;
        state.focus.reset ();
        state.warning.clear ();
        return true;
    }
    if (kind == "button" && id == UiId (state, "showAll")) {
        state.focus.reset ();
        return true;
    }
    for (const auto& row : presentation.rows) {
        if (kind == "checkbox" && id == UiId (state, "choose", row.index) && (value == 0 || value == 1)) {
            if (value == 1)
                state.pending.insert (row.index);
            else
                state.pending.erase (row.index);
            return true;
        }
        if (kind == "swatch" && id == UiId (state, "highlight", row.index) && state.selected.contains (row.index)) {
            if (state.focus == row.index)
                state.focus.reset ();
            else
                state.focus = row.index;
            return true;
        }
    }
    return false;
}
std::map<int32_t, double> UiAreas (const Presentation& presentation, const UiState& state)
{
    std::map<int32_t, double> areas;
    for (const auto& row : presentation.rows)
        areas[row.index] = 0;
    for (size_t i = 0; i < presentation.meshSurfaces.size (); ++i)
        if (Included (presentation.meshFacings[i], state) && MatchesCriteria (presentation, state, i))
            areas[presentation.meshSurfaces[i]] += presentation.meshAreas[i];
    return areas;
}
layers::Layer UiLayer (const Presentation& presentation, const UiState& state)
{
    auto layer = presentation.layer;
    layer.meshes.clear ();
    layer.panels.clear ();
    for (size_t i = 0; i < presentation.meshSurfaces.size (); ++i) {
        const int32_t surface = presentation.meshSurfaces[i];
        if (state.selected.contains (surface) && Included (presentation.meshFacings[i], state) &&
            MatchesCriteria (presentation, state, i) && (!state.focus || state.focus == surface))
            layer.meshes.push_back (presentation.layer.meshes[i]);
    }
    layers::Panel panel;
    panel.title = "Exposed surfaces";
    panel.widthPixels = 450;
    layers::ApplyTheme (panel, layers::PanelTheme::Light);
    const auto text = [&] (const std::string& value) {
        layers::PanelItem item;
        item.text = value;
        item.wrap = true;
        panel.items.push_back (item);
    };
    text (presentation.scopeLabel);
    text ("Object selection: " +
          (state.rules.empty () ? std::string ("all captured candidates")
                                : std::to_string (state.rules.size ()) + " applied criteria") +
          ". Edit rules in the Surface criteria tab.");
    text ("Contact gap: " + Area (presentation.contactToleranceM * 1000) +
          " mm. Touching/intersecting covers do not require SEO.");
    if (!state.warning.empty ())
        text (state.warning);
    for (const auto& entry : { std::pair { "top", std::pair { "Include top-facing faces", state.includeTop } },
                               std::pair { "bottom", std::pair { "Include bottom-facing faces", state.includeBottom } },
                               std::pair { "sides", std::pair { "Include side-facing faces", state.includeSides } } }) {
        layers::PanelItem toggle;
        toggle.kind = layers::ItemKind::Checkbox;
        toggle.id = UiId (state, entry.first);
        toggle.text = entry.second.first;
        toggle.checked = entry.second.second;
        panel.items.push_back (toggle);
    }
    layers::PanelItem section;
    section.kind = layers::ItemKind::Section;
    section.text = "Materials - choose a list";
    section.open = false;
    panel.items.push_back (section);
    for (const auto& row : presentation.rows) {
        layers::PanelItem choose;
        choose.kind = layers::ItemKind::Checkbox;
        choose.id = UiId (state, "choose", row.index);
        choose.text = row.name;
        choose.checked = state.pending.contains (row.index);
        panel.items.push_back (choose);
    }
    layers::PanelItem button;
    button.kind = layers::ItemKind::Button;
    button.id = UiId (state, "calculate");
    button.text = "Calculate selected materials";
    panel.items.push_back (button);
    section.text = "Surface areas";
    section.open = true;
    panel.items.push_back (section);
    text ("Click a color swatch to isolate its highlight. Click again to show the list.");
    const auto areas = UiAreas (presentation, state);
    double total = 0;
    for (const auto& row : presentation.rows)
        if (state.selected.contains (row.index)) {
            layers::PanelItem swatch;
            swatch.kind = layers::ItemKind::Swatch;
            swatch.id = UiId (state, "highlight", row.index);
            swatch.rgba = row.rgba;
            swatch.checked = !state.focus || state.focus == row.index;
            swatch.text = row.name;
            swatch.value = Area (areas.at (row.index)) + " m2";
            panel.items.push_back (swatch);
            total += areas.at (row.index);
        }
    layers::PanelItem sum;
    sum.kind = layers::ItemKind::Row;
    sum.text = "Measured total";
    sum.value = Area (total) + " m2";
    sum.sizePixels = 19;
    panel.items.push_back (sum);
    button.id = UiId (state, "showAll");
    button.text = "Highlight all selected materials";
    panel.items.push_back (button);
    section.text = "Native comparison (different rules)";
    section.open = false;
    panel.items.push_back (section);
    layers::PanelItem table;
    table.kind = layers::ItemKind::Table;
    table.columns = { "Surface", "Native whole walls m2" };
    for (const auto& row : presentation.rows)
        if (state.selected.contains (row.index)) {
            const auto native = presentation.nativeAreas.find (row.index);
            table.rows.push_back (
                { row.name, native == presentation.nativeAreas.end () ? "unavailable" : Area (native->second) });
        }
    panel.items.push_back (table);
    text (presentation.comparisonNote);
    section.text = "Coverage and refresh";
    section.open = false;
    panel.items.push_back (section);
    text (presentation.coverLabel);
    text ("Top/bottom: normals within 45 degrees of world up/down; other faces are sides. Cover geometry is never "
          "filtered. Areas use this capture's validated fragments; refresh after edits.");
    layer.panels.push_back (std::move (panel));
    layer.panels.push_back (CriteriaPanel (presentation, state));
    return layer;
}
} // namespace geomsrv::archviz::surfacetakeoff
