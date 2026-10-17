// Pure schedule-style criteria draft/application and native panel content.
#include "SurfaceTakeoffLayer.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::surfacetakeoff {
namespace sc = surfacecriteria;
std::vector<std::string> CriterionValues (const Presentation& presentation, sc::Field field)
{
    std::set<std::string> values;
    for (const auto& [guid, element] : presentation.elements) {
        switch (field) {
            case sc::Field::ElementType:
                values.insert (element.type);
                break;
            case sc::Field::Layer:
                if (element.detailsAvailable)
                    values.insert (element.layer);
                break;
            case sc::Field::HomeStory:
                if (element.detailsAvailable)
                    values.insert (element.story);
                break;
            case sc::Field::ElementId:
                if (element.detailsAvailable)
                    values.insert (element.id);
                break;
            default:
                break;
        }
    }
    if (field == sc::Field::Surface)
        for (const auto& row : presentation.rows)
            values.insert (row.name);
    if (field == sc::Field::Facing)
        values = { "Top", "Bottom", "Side" };
    std::vector<std::string> result;
    for (const auto& value : values) {
        if (result.size () == 64)
            break;
        result.push_back (value);
    }
    return result;
}
bool ApplyCriteriaChange (const Presentation& presentation, UiState& state, const std::string& kind,
                          const std::string& id, double value, const std::string& text)
{
    if (kind == "button") {
        if (id == UiId (state, "applyCriteria")) {
            state.criteriaError = sc::Validate (state.draftRules);
            if (state.criteriaError.empty ()) {
                state.rules = state.draftRules;
                state.focus.reset ();
            }
            return true;
        }
        if (id == UiId (state, "clearCriteria")) {
            state.draftRules.clear ();
            state.criteriaError.clear ();
            return true;
        }
        if (id == UiId (state, "addCriterion") && state.draftRules.size () < sc::kMaxRules) {
            sc::Rule rule;
            rule.id = state.nextRuleId++;
            auto values = CriterionValues (presentation, rule.field);
            rule.value = std::find (values.begin (), values.end (), "Wall") != values.end () ? "Wall"
                         : values.empty ()                                                   ? ""
                                                                                             : values.front ();
            state.draftRules.push_back (rule);
            return true;
        }
    }
    for (size_t i = 0; i < state.draftRules.size (); ++i) {
        auto& rule = state.draftRules[i];
        if (id != UiId (state, "rule", rule.id))
            continue;
        if (!std::isfinite (value) || std::floor (value) != value)
            return false;
        if (kind == "criteriaRemove") {
            state.draftRules.erase (state.draftRules.begin () + i);
            return true;
        }
        if (kind == "criteriaMove" && (value == -1 || value == 1)) {
            const auto next = int64_t (i) + int64_t (value);
            if (next >= 0 && next < int64_t (state.draftRules.size ()))
                std::swap (rule, state.draftRules[size_t (next)]);
            return true;
        }
        if (kind == "criteriaField" && value >= 0 && value < sc::Fields ().size ()) {
            rule.field = sc::Field (int (value));
            const auto choices = CriterionValues (presentation, rule.field);
            rule.value = choices.empty () ? "" : choices.front ();
            return true;
        }
        if (kind == "criteriaCondition" && value >= 0 && value < sc::Conditions ().size ()) {
            rule.condition = sc::Condition (int (value));
            return true;
        }
        if (kind == "criteriaValue" && value == int (rule.field) && text.size () <= 512) {
            rule.value = text;
            return true;
        }
        if (kind == "criteriaLink" && (value == 0 || value == 1)) {
            rule.orNext = value == 1;
            return true;
        }
        if (kind == "criteriaOpen" && value >= 0 && value <= 3) {
            rule.open = uint32_t (value);
            return true;
        }
        if (kind == "criteriaClose" && value >= 0 && value <= 3) {
            rule.close = uint32_t (value);
            return true;
        }
    }
    return false;
}
bool MatchesCriteria (const Presentation& presentation, const UiState& state, size_t mesh)
{
    const auto element = presentation.elements.find (presentation.meshElements[mesh]);
    const auto surface = std::find_if (presentation.rows.begin (), presentation.rows.end (),
                                       [&] (const auto& row) { return row.index == presentation.meshSurfaces[mesh]; });
    if (element == presentation.elements.end () || surface == presentation.rows.end ())
        return false;
    const auto facing = presentation.meshFacings[mesh];
    return sc::Matches (state.rules, element->second, surface->name,
                        facing == Facing::Top      ? "Top"
                        : facing == Facing::Bottom ? "Bottom"
                                                   : "Side");
}
overlaylayers::Panel CriteriaPanel (const Presentation& presentation, const UiState& state)
{
    namespace layers = overlaylayers;
    layers::Panel panel;
    panel.title = "Surface criteria";
    panel.widthPixels = panel.pageWidthPixels = 1000;
    layers::ApplyTheme (panel, layers::PanelTheme::Light);
    layers::PanelItem text;
    text.wrap = true;
    text.text = "Object/surface criteria - AND/OR with parentheses. Draft rows do not change quantities until Apply "
                "criteria. Covers remain independent.";
    panel.items.push_back (text);
    layers::PanelItem grid;
    grid.kind = layers::ItemKind::CriteriaGrid;
    grid.id = UiId (state, "criteriaGrid");
    grid.labels = sc::Fields ();
    grid.columns = sc::Conditions ();
    for (const auto& rule : state.draftRules) {
        layers::CriteriaRow row;
        row.id = UiId (state, "rule", rule.id);
        row.field = uint32_t (rule.field);
        row.condition = uint32_t (rule.condition);
        row.open = rule.open;
        row.close = rule.close;
        row.orNext = rule.orNext;
        row.value = rule.value;
        row.options = CriterionValues (presentation, rule.field);
        grid.criteria.push_back (row);
    }
    panel.items.push_back (grid);
    for (const auto& entry : { std::pair { "addCriterion", "Add criterion" },
                               { "clearCriteria", "Clear draft" },
                               { "applyCriteria", "Apply criteria" } }) {
        layers::PanelItem button;
        button.kind = layers::ItemKind::Button;
        button.id = UiId (state, entry.first);
        button.text = entry.second;
        panel.items.push_back (button);
    }
    text.text = state.criteriaError.empty () ? sc::Validate (state.draftRules) : state.criteriaError;
    if (!text.text.empty ()) {
        text.rgba = 0xC43D3DFFu;
        panel.items.push_back (text);
    }
    text.rgba = 0;
    text.text =
        "Supported: Element Type, Layer, Home Story, Element ID, Surface, face direction. Type a value (Enter to "
        "commit), or choose a captured value. No IFC/custom-property criteria or native schedule editing is claimed.";
    panel.items.push_back (text);
    return panel;
}
} // namespace geomsrv::archviz::surfacetakeoff
