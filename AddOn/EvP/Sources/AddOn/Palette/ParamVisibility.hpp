#ifndef GEOMETRYSERVER_PALETTE_PARAMVISIBILITY_HPP
#define GEOMETRYSERVER_PALETTE_PARAMVISIBILITY_HPP

// F3 — which generated parameter rows a command's show_when leaves on screen.
//
// DELIBERATELY FREE OF THE DEVKIT, exactly like Palette/CommandFilter: strings in,
// flags out, no DG, no ACAPI, no GS::UniString. Visibility is the half of F3 that
// can be wrong SILENTLY — a row that quietly never appears looks like a command
// that forgot a setting — so it is covered by the offline C++ suite
// (tests/cpp/test_param_visibility.cpp) rather than judged by eye in Archicad.
// ParamPanel converts its UniStrings at the boundary and calls in.
//
// Values are compared as exact strings. The scanner normalises both sides to the
// same spelling (a bool is "true"/"false"), so there is nothing to guess here.

#include <string>
#include <vector>

namespace evp {

// One row's declared dependency: show it only while `controller` holds one of
// `values`. An empty controller means the row is unconditional, which is what
// almost every row is.
struct VisibilityRule {
    std::string              controller;
    std::vector<std::string> values;
};

// `names`, `values` and `rules` are parallel: one entry per parameter row, in
// declaration order. Returns one flag per row.
//
// CHAINS RESOLVE. A row whose controller is ITSELF hidden is hidden too, however
// deep the chain runs — otherwise switching the action away from "Place" would
// hide the mode popup while leaving the input that only existed for one of its
// modes sitting there, controlled by something invisible.
//
// A rule naming a parameter that does not exist leaves its row VISIBLE. The
// scanner already refuses that case loudly, and if one ever slips through, a row
// that shows when it should not is reportable — one that vanishes is not.
std::vector<bool> EvaluateVisibility (const std::vector<std::string>&    names,
                                      const std::vector<std::string>&    values,
                                      const std::vector<VisibilityRule>& rules);

}   // namespace evp

#endif
