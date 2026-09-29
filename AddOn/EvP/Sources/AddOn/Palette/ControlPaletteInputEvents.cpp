#include "ControlPalette.hpp"

// Generated parameter events live together: each refreshes the Run gate and
// reflows when show_when changes the row assembly.

void ControlPalette::TextEditChanged (const DG::TextEditChangeEvent& /*ev*/)
{
    // A required text parameter is empty until the user types something, so the
    // gate has to react to typing. Relying on the idle poll made a required field
    // look broken: you could type a name and Run stayed disabled.
    RefreshRunGate ();
    RefreshSearchFilter (); // the search box commits through here too
}

void ControlPalette::UserControlChanged (const DG::UserControlChangeEvent& /*ev*/)
{
    // Pen swatches only. ACAPI_Dialog_SetUserControlCallback stores the new pen
    // on the item, which ParamPanel::CollectJson reads at Run time.
}

void ControlPalette::PopUpChanged (const DG::PopUpChangeEvent& ev)
{
    bool reflow = false;
    if (!preview.HandlePopUpChanged (ev) && params.HandlePopUpChanged (ev, reflow)) {
        if (reflow)
            ReflowParams ();
        RefreshRunGate ();
    }
}

void ControlPalette::DateTimeChanged (const DG::DateTimeChangeEvent& ev)
{
    bool reflow = false;
    if (params.HandleDateTimeChanged (ev, reflow)) {
        if (reflow)
            ReflowParams ();
        RefreshRunGate ();
    }
}
