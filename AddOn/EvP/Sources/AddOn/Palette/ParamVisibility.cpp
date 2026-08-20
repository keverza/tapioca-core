#include "Palette/ParamVisibility.hpp"

#include <algorithm>
#include <cstddef>

namespace evp {

std::vector<bool> EvaluateVisibility (const std::vector<std::string>&    names,
                                      const std::vector<std::string>&    values,
                                      const std::vector<VisibilityRule>& rules)
{
    const std::size_t count = rules.size ();
    std::vector<bool> visible (count, true);

    // Pass 1 — each rule against the value its controller currently holds.
    for (std::size_t i = 0; i < count; ++i) {
        const VisibilityRule& rule = rules[i];
        if (rule.controller.empty ())
            continue;

        const auto found = std::find (names.begin (), names.end (), rule.controller);
        if (found == names.end ())
            continue;                   // unknown controller: leave it visible, see the header

        const std::size_t index = (std::size_t) (found - names.begin ());
        const std::string current = (index < values.size ()) ? values[index] : std::string ();
        visible[i] = std::find (rule.values.begin (), rule.values.end (), current) != rule.values.end ();
    }

    // Pass 2 — propagate along the chains. A row controlled by a hidden row is
    // hidden too. Each sweep can only ever turn flags OFF, so the loop terminates:
    // at worst one row per sweep, hence the `count` bound, and in practice one
    // sweep settles it because chains are one or two deep.
    for (std::size_t sweep = 0; sweep < count; ++sweep) {
        bool changed = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (!visible[i] || rules[i].controller.empty ())
                continue;
            const auto found = std::find (names.begin (), names.end (), rules[i].controller);
            if (found == names.end ())
                continue;
            const std::size_t index = (std::size_t) (found - names.begin ());
            if (index < count && !visible[index]) {
                visible[i] = false;
                changed    = true;
            }
        }
        if (!changed)
            break;
    }

    return visible;
}

}   // namespace evp
