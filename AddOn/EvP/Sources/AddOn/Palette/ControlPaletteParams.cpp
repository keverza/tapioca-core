#include "ControlPalette.hpp"

void ControlPalette::ReflowParams ()
{
    Layout ();
    params.ShowControls ();
    Redraw ();
}
