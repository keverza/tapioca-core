#include "Palette/ParamPanel.hpp"
#include "Palette/ParamVisibility.hpp"
#include "Palette/ParamValues.hpp"

namespace evp {

// Keep reactive per-row metadata out of the control factory. Visibility and
// advisory text can ask for a reflow, but neither changes parameter values.
bool ParamPanel::ApplyVisibility ()
{
    std::vector<std::string> names, values;
    std::vector<VisibilityRule> rules;
    names.reserve (paramControls.size ());
    values.reserve (paramControls.size ());
    rules.reserve (paramControls.size ());
    for (const ParamControl& pc : paramControls) {
        names.push_back (Utf8 (pc.name));
        values.push_back (Utf8 (pc.CurrentValueText ()));
        VisibilityRule rule;
        rule.controller = Utf8 (pc.showWhenParam);
        for (const auto& value : pc.showWhenValues)
            rule.values.push_back (Utf8 (value));
        rules.push_back (std::move (rule));
    }
    const auto visible = EvaluateVisibility (names, values, rules);
    bool changed = false;
    for (size_t i = 0; i < paramControls.size (); ++i) {
        if (paramControls[i].visible != visible[i]) {
            paramControls[i].visible = visible[i];
            changed = true;
        }
    }
    return changed;
}

bool ParamPanel::RefreshAdvisories ()
{
    bool changed = false;
    for (auto& pc : paramControls) {
        if (pc.advisory)
            changed = pc.advisory->Refresh (pc.CurrentValueText ()) || changed;
    }
    return changed;
}

} // namespace evp
