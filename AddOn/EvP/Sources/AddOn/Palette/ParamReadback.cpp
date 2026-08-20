#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/ParamPanel.hpp"
#include "Palette/ParamValues.hpp"         // EscapeJson / JsonReal — how a value is spelled
#include "NativeCommands/CommandUtils.hpp" // geomsrv::AttributeIndexToName

// READING THE GENERATED CONTROLS BACK — the other half of ParamPanel.
//
// ParamPanel BUILDS a control per scanned parameter and PLACES it. Everything
// here goes the other way: what a live control currently holds, whether that
// counts as a value at all, what a show_when compares against, and the JSON
// run() is finally called with.
//
// Split out when the evp.LibraryPart / evp.Favourite pickers pushed ParamPanel
// past the soft cap, and the right seam regardless: every new parameter kind
// adds one case to each of the four functions below, so they grow together and
// they grow as a set. A kind that is added to CollectJson but not to HasValue is
// a required control that can never gate Run — keeping them in one file is what
// makes that omission visible.
//
// ⚠️ NOT in Palette/ParamValues.cpp, which states that nothing in it touches DG.
// These four all cast to a concrete DG control, which is exactly the knowledge
// ParamValues exists without.

namespace evp {

bool ParamControl::HasValue () const
{
    switch (kind) {
        case Kind::Attribute:
            // An attribute picker that resolved to nothing is the "layer is
            // missing" case: it looks filled in but names nothing real.
            return picker != nullptr && picker->GetSelectedAttributeIndex ().ToInt32_Deprecated () > 0;
        case Kind::Enum:
        case Kind::Action: {
            // The selection must land on a real CHOICE, not merely on a row. The
            // attribute picker's popup fallback appends a "(no attributes in this
            // project)" row that is deliberately absent from `choices` so it reads
            // back as "" — without this the placeholder would satisfy a required
            // parameter and the command would start on an empty value. The other
            // project-filled popups (Story, NavItem, ProjectField) answer the same
            // question against their own value arrays, below.
            const short selected = static_cast<DG::PopUp*> (control.get ())->GetSelectedItem ();
            return selected >= 1 && (UIndex) selected <= choices.GetSize ();
        }
        case Kind::Story:
            // A project with no stories gets a placeholder row with no index behind
            // it — that is the "nothing to choose" case, same shape as an empty enum.
            return !storyIndices.IsEmpty ();
        case Kind::NavItem:
            // Same shape: the fallback row of a project with no databases has no
            // guid behind it, so a REQUIRED parameter must gate Run rather than let
            // the command start and fail on an empty guid.
            return !itemGuids.IsEmpty ();
        case Kind::ProjectField:
            // Same shape again: a project with no Project Info fields gets a
            // placeholder row with no KEY behind it. Empty means "nothing to
            // choose", which must gate Run rather than send an empty field name.
            return !choiceValues.IsEmpty ();
        case Kind::NavBrowse:
            // A View button starts empty and stays empty until the user opens the
            // browser and chooses. That is the whole point of gating Run on it.
            return !selectedGuid.IsEmpty ();
        case Kind::Catalog:
            // Same shape: a LibraryPart/Favourite button holds nothing until the
            // catalogue browser returns something. The VALUE is what is checked,
            // not the label — a label with no value behind it is the failure this
            // gate exists for.
            return !selectedValueJson.IsEmpty ();
        case Kind::Text:
        case Kind::FilePath:
            return !static_cast<DG::TextEdit*> (control.get ())->GetText ().IsEmpty ();
        default:
            // Numbers, bools and pens always hold something usable.
            return true;
    }
}

// What a show_when compares against. Only the CLOSED kinds answer — an Action, an
// Enum, a checkbox — because those are the ones the scanner can validate a
// show_when against. A text field answers too (it costs nothing and reads
// naturally), and everything else returns empty, which no rule can match.
GS::UniString ParamControl::CurrentValueText () const
{
    switch (kind) {
        case Kind::Bool:
            // "true"/"false" — the spelling _scanner._value_text() normalises to.
            return static_cast<DG::CheckBox*> (control.get ())->IsChecked () ? "true" : "false";
        case Kind::Enum:
        case Kind::Action: {
            const short selected = static_cast<DG::PopUp*> (control.get ())->GetSelectedItem ();
            return (selected >= 1 && (UIndex) selected <= choices.GetSize ()) ? choices[selected - 1]
                                                                              : GS::UniString ();
        }
        case Kind::NavBrowse:
            return selectedGuid;
        case Kind::Catalog:
            // The chosen row's NAME. Unlike a guid this one CAN be written as a
            // literal in a command's source, so show_when={"door": "..."} is
            // genuinely usable — and "" still matches "nothing chosen yet".
            return selectedLabel;
        case Kind::NavItem: {
            // What a show_when compares against is the value the parameter SENDS —
            // the guid, not the row text — so the two can never disagree. In
            // practice only the empty string is matchable, because a guid cannot be
            // written as a literal in a command's source.
            const short selected = static_cast<DG::PopUp*> (control.get ())->GetSelectedItem ();
            return (selected >= 1 && (UIndex) selected <= itemGuids.GetSize ()) ? itemGuids[selected - 1]
                                                                                : GS::UniString ();
        }
        case Kind::ProjectField: {
            // The KEY, for the same reason: a show_when compares against the value
            // the parameter sends. Unlike a guid a key CAN be written as a literal
            // in a command's source, so show_when={"plot_field": "SKLYPOPLOTAS"} is
            // genuinely usable here.
            const short selected = static_cast<DG::PopUp*> (control.get ())->GetSelectedItem ();
            return (selected >= 1 && (UIndex) selected <= choiceValues.GetSize ())
                       ? choiceValues[selected - 1] : GS::UniString ();
        }
        case Kind::Text:
        case Kind::FilePath:
            return static_cast<DG::TextEdit*> (control.get ())->GetText ();
        default:
            return GS::UniString ();
    }
}

// Reads the generated controls back into the JSON run() is called with.
//
// EVERY parameter, visible or not (F3). run() is called with its whole signature,
// so a hidden row sends whatever it currently holds — its default — and the
// command's action branch ignores what it does not use. Dropping the key instead
// would TypeError on any parameter without a default, and would make a command's
// signature depend on what the palette happened to be showing.
GS::UniString ParamPanel::CollectJson () const
{
    GS::UniString json ("{");
    bool first = true;

    for (const ParamControl& pc : paramControls) {
        if (!first)
            json += ",";
        first = false;
        json += "\"" + EscapeJson (pc.name) + "\":";

        switch (pc.kind) {
            case ParamControl::Kind::Bool:
                json += static_cast<DG::CheckBox*> (pc.control.get ())->IsChecked () ? "true" : "false";
                break;
            case ParamControl::Kind::Int:
                json += GS::UniString::Printf ("%d", (int) static_cast<DG::IntEdit*> (pc.control.get ())->GetValue ());
                break;
            case ParamControl::Kind::Real:
            case ParamControl::Kind::Length:
            case ParamControl::Kind::Area:
            case ParamControl::Kind::Volume:
            case ParamControl::Kind::Angle:
                json += JsonReal (static_cast<DG::RealEdit*> (pc.control.get ())->GetValue ());
                break;
            case ParamControl::Kind::Enum:
            case ParamControl::Kind::Action: {
                const short selected = static_cast<DG::PopUp*> (pc.control.get ())->GetSelectedItem ();
                const GS::UniString value = (selected >= 1 && (UIndex) selected <= pc.choices.GetSize ())
                                                ? pc.choices[selected - 1]
                                                : GS::UniString ();
                json += "\"" + EscapeJson (value) + "\"";
                break;
            }
            case ParamControl::Kind::Pen:
                // A pen is a number 1..255, not a named attribute.
                json += GS::UniString::Printf ("%d", (int) pc.penSwatch->GetValue ());
                break;
            case ParamControl::Kind::Story: {
                // The story INDEX behind the selected row, NOT the row number.
                const short selected = static_cast<DG::PopUp*> (pc.control.get ())->GetSelectedItem ();
                const GS::Int32 index = (selected >= 1 && (UIndex) selected <= pc.storyIndices.GetSize ())
                                            ? pc.storyIndices[selected - 1]
                                            : 0;
                json += GS::UniString::Printf ("%d", (int) index);
                break;
            }
            case ParamControl::Kind::NavBrowse:
                // Whatever the browser last returned. Empty until the user picks,
                // which is exactly the state a required View parameter blocks Run on.
                json += "\"" + EscapeJson (pc.selectedGuid) + "\"";
                break;
            case ParamControl::Kind::Catalog:
                // ⚠️ THE ONE KIND THAT EMITS AN OBJECT, NOT A SCALAR — already
                // built and escaped by HandleButtonClicked, so it goes in
                // verbatim. `null` (not "") while nothing is chosen: an OPTIONAL
                // picker has to reach run() as the None its signature defaults
                // to, and an empty string would look like a chosen thing with a
                // blank name.
                json += pc.selectedValueJson.IsEmpty () ? GS::UniString ("null") : pc.selectedValueJson;
                break;
            case ParamControl::Kind::NavItem: {
                // The GUID behind the selected row, NOT the row text. The fallback
                // row of an empty project has no guid, so it lands here as "" —
                // which is what the command is expected to refuse on.
                const short selected = static_cast<DG::PopUp*> (pc.control.get ())->GetSelectedItem ();
                const GS::UniString guid = (selected >= 1 && (UIndex) selected <= pc.itemGuids.GetSize ())
                                               ? pc.itemGuids[selected - 1]
                                               : GS::UniString ();
                json += "\"" + EscapeJson (guid) + "\"";
                break;
            }
            case ParamControl::Kind::ProjectField: {
                // The DATABASE KEY behind the selected row, NOT the description the
                // row shows. The placeholder row of a project with no fields has no
                // key, so it lands here as "" — which is what the command refuses on.
                const short selected = static_cast<DG::PopUp*> (pc.control.get ())->GetSelectedItem ();
                const GS::UniString key = (selected >= 1 && (UIndex) selected <= pc.choiceValues.GetSize ())
                                              ? pc.choiceValues[selected - 1]
                                              : GS::UniString ();
                json += "\"" + EscapeJson (key) + "\"";
                break;
            }
            case ParamControl::Kind::FilePath:
                json += "\"" + EscapeJson (static_cast<DG::TextEdit*> (pc.control.get ())->GetText ()) + "\"";
                break;
            case ParamControl::Kind::Attribute: {
                if (pc.picker == nullptr) {
                    json += "\"\"";
                    break;
                }
                // Named attributes travel as their NAME: it matches the declared
                // default, reads well in a log, and survives being written down.
                // Scripts resolve name -> index when they need it.
                const API_AttributeIndex index = pc.picker->GetSelectedAttributeIndex ();
                json += "\"" + EscapeJson (geomsrv::AttributeIndexToName (pc.attrType, index)) + "\"";
                break;
            }
            case ParamControl::Kind::Text:
            default:
                json += "\"" + EscapeJson (static_cast<DG::TextEdit*> (pc.control.get ())->GetText ()) + "\"";
                break;
        }
    }
    json += "}";
    return json;
}

// A required parameter with nothing usable in it. Empty == this panel is ready;
// the shell adds the reasons that are not about parameters.
//
// INVISIBLE ROWS ARE IGNORED (F3), for the same reason a readonly one is: a
// required parameter the user cannot see would disable Run with a message naming
// a control that is not on screen — a dead end with no way out of it.
GS::UniString ParamPanel::WhatIsMissing () const
{
    for (const ParamControl& pc : paramControls) {
        if (pc.visible && pc.required && !pc.HasValue ())
            return "Choose a " + pc.name + " — it has no value yet.";
    }
    return GS::UniString ();
}

} // namespace evp
