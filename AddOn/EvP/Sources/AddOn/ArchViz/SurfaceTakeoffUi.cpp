// OVERLAY-INVARIANTS.md: pure layer content/state, no renderer or camera changes.
#include "SurfaceTakeoffLayer.hpp"
#include <algorithm>
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
    (void) presentation;
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
            state.areaPage = 0;
            state.focus.reset ();
            return true;
        }
    }
    if (kind == "button" && (id == UiId (state, "previousAreas") || id == UiId (state, "nextAreas"))) {
        const auto areas = UiAreas (presentation, state);
        const size_t count =
            size_t (std::count_if (areas.begin (), areas.end (), [] (const auto& row) { return row.second > 0; }));
        const size_t last = count ? (count - 1) / kAreaPageSize : 0;
        state.areaPage = (std::min) (state.areaPage, last);
        if (id == UiId (state, "previousAreas") && state.areaPage > 0)
            --state.areaPage;
        else if (id == UiId (state, "nextAreas") && state.areaPage < last)
            ++state.areaPage;
        return true;
    }
    if (kind == "button" && id == UiId (state, "showAll")) {
        state.focus.reset ();
        return true;
    }
    for (const auto& row : presentation.rows) {
        if (kind == "swatch" && id == UiId (state, "highlight", row.index) &&
            UiAreas (presentation, state).at (row.index) > 0) {
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
    layer.polylines.clear ();
    layer.panels.clear ();
    std::vector<size_t> shown;
    for (size_t i = 0; i < presentation.meshSurfaces.size (); ++i) {
        const int32_t surface = presentation.meshSurfaces[i];
        if (Included (presentation.meshFacings[i], state) && MatchesCriteria (presentation, state, i) &&
            (!state.focus || state.focus == surface)) {
            layer.meshes.push_back (presentation.layer.meshes[i]);
            shown.push_back (i);
        }
    }
    std::string outlineError;
    BoundaryLines (presentation, shown, layer.polylines, outlineError);
    layers::Panel panel;
    panel.title = "Exposed surfaces";
    panel.widthPixels = panel.pageWidthPixels = 1000;
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
          ".");
    text ("Contact gap: " + Area (presentation.contactToleranceM * 1000) +
          " mm. Touching/intersecting covers do not require SEO.");
    if (!state.warning.empty ())
        text (state.warning);
    if (!outlineError.empty ())
        text ("Highlight outlines unavailable: " + outlineError + " Measured fragments are unchanged.");
    layers::PanelItem section;
    section.kind = layers::ItemKind::Section;
    section.text = "Surface criteria";
    section.open = true;
    panel.items.push_back (section);
    AppendCriteria (panel, presentation, state);
    section.text = "Face directions";
    section.open = true;
    panel.items.push_back (section);
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
    section.text = "Surface areas";
    section.open = true;
    panel.items.push_back (section);
    text ("Click a swatch to isolate that surface. Click again to show all matching surfaces. Zero areas are hidden.");
    const auto areas = UiAreas (presentation, state);
    double total = 0;
    std::vector<const SurfaceRow*> visible;
    for (const auto& row : presentation.rows) {
        total += areas.at (row.index);
        if (areas.at (row.index) > 0)
            visible.push_back (&row);
    }
    const size_t pages = (std::max) (size_t (1), (visible.size () + kAreaPageSize - 1) / kAreaPageSize);
    const size_t page = (std::min) (state.areaPage, pages - 1);
    const size_t begin = page * kAreaPageSize, end = (std::min) (begin + kAreaPageSize, visible.size ());
    if (visible.empty ())
        text ("No exposed surfaces match the applied criteria and face directions.");
    else {
        text (std::to_string (visible.size ()) + " matching surfaces. Page " + std::to_string (page + 1) + " / " +
              std::to_string (pages));
        for (size_t i = begin; i < end; ++i) {
            const auto& row = *visible[i];
            layers::PanelItem swatch;
            swatch.kind = layers::ItemKind::Swatch;
            swatch.id = UiId (state, "highlight", row.index);
            swatch.rgba = kHighlightRgba;
            swatch.checked = !state.focus || state.focus == row.index;
            swatch.text = row.name;
            swatch.value =
                (areas.at (row.index) < .0005 ? "<0.001" : Area (areas.at (row.index))) + std::string (" m2");
            panel.items.push_back (swatch);
        }
    }
    layers::PanelItem button;
    button.kind = layers::ItemKind::Button;
    if (page > 0) {
        button.id = UiId (state, "previousAreas");
        button.text = "Previous surfaces";
        panel.items.push_back (button);
    }
    if (page + 1 < pages) {
        button.id = UiId (state, "nextAreas");
        button.text = "Next surfaces";
        panel.items.push_back (button);
    }
    layers::PanelItem sum;
    sum.kind = layers::ItemKind::Row;
    sum.text = "Measured total";
    sum.value = Area (total) + " m2";
    sum.sizePixels = 19;
    panel.items.push_back (sum);
    button.id = UiId (state, "showAll");
    button.text = "Highlight all matching surfaces";
    panel.items.push_back (button);
    section.text = "Native comparison (different rules)";
    section.open = false;
    panel.items.push_back (section);
    layers::PanelItem table;
    table.kind = layers::ItemKind::Table;
    table.columns = { "Surface", "Native whole elements m2" };
    for (size_t i = begin; i < end; ++i) {
        const auto& row = *visible[i];
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
    return layer;
}
} // namespace geomsrv::archviz::surfacetakeoff
