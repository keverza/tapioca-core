// The palette's registration with Archicad's modeless-window system, and the
// message pump that comes with it.
//
// This is Archicad-integration plumbing — a switch on API_PaletteMessageID — not
// palette layout work: it decides nothing about bands or items, it only relays
// "show", "hide" and "I am busy" to the one instance. It stays in Palette/ beside
// the shell because it defines the shell's registration callbacks.
//
// The definitions stay ControlPalette::* so the header, the GUID identity and the
// callback signature are unchanged; only the file they live in moved.

#include "ControlPalette.hpp"
#include "Python/PathUtils.hpp"

#include "ACAPinc.h"

// The palette's identity, shared with its constructor (Palette/ControlPalette.cpp).
// Declared there, defined there; ACAPI keys the modeless window on its hash.
extern const GS::Guid paletteGuid;

void ControlPalette::Show (bool focusSearch)
{
    paletteState.RequestOpen ();
    if (!paletteState.CanShow ())
        return;
    preview.SetPaletteVisible (true);
    DG::Palette::Show ();
    if (focusSearch)
        commandsPanel.FocusSearch ();
}

void ControlPalette::Hide ()
{
    paletteState.RequestClose ();
    CancelAutomaticPreview (true);
    preview.SetPaletteVisible (false);
    DG::Palette::Hide ();
}

GSErrCode ControlPalette::PaletteControlCallBack (Int32, API_PaletteMessageID messageID, GS::IntPtr param)
{
    switch (messageID) {
        case APIPalMsg_OpenPalette:
            // Archicad can replay this from its saved workspace. Only the
            // add-on's explicit menu/API open authorizes a palette this session.
            if (HasInstance () && GetInstance ().paletteState.CanShow ())
                GetInstance ().Show (true);
            break;
        case APIPalMsg_ClosePalette:
            if (HasInstance ())
                GetInstance ().Hide ();
            break;
        case APIPalMsg_HidePalette_Begin:
            if (HasInstance ()) {
                auto& panel = GetInstance ();
                panel.paletteState.BeginHostHide (panel.IsVisible ());
                panel.CancelAutomaticPreview (true);
                panel.preview.SetPaletteVisible (false);
                panel.DG::Palette::Hide ();
            }
            break;
        case APIPalMsg_HidePalette_End:
            if (HasInstance () && GetInstance ().paletteState.EndHostHide ()) {
                GetInstance ().preview.SetPaletteVisible (true);
                GetInstance ().DG::Palette::Show ();
            }
            break;

        // Archicad telling us it is busy — opening a project, mostly. The FLAG
        // matters as much as the greying: the palette's idle poll reads attribute
        // PICKERS back, and those are bound to the project's attribute set. Polling
        // one while that set is swapped underneath us is the shape of bug that takes
        // the whole application down without leaving a log line. So while this is
        // set, ControlPalette::PanelIdle does nothing at all.
        case APIPalMsg_DisableItems_Begin:
            if (HasInstance ()) {
                GetInstance ().itemsDisabled.store (true);
                if (GetInstance ().IsVisible ())
                    GetInstance ().DisableItems ();
            }
            break;
        case APIPalMsg_DisableItems_End:
            if (HasInstance ()) {
                GetInstance ().itemsDisabled.store (false);
                if (GetInstance ().IsVisible ())
                    GetInstance ().EnableItems ();
            }
            break;

        case APIPalMsg_IsPaletteVisible:
            *(reinterpret_cast<bool*> (param)) = HasInstance () && GetInstance ().IsVisible ();
            break;
        case APIPalMsg_GetPaletteDeactivationMethod:
            *(reinterpret_cast<API_PaletteDeactivationMethod*> (param)) = APIPaletteDeactivationMethod_Default;
            break;
        default:
            break;
    }
    return NoError;
}

GSErrCode ControlPalette::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (GS::CalculateHashValue (paletteGuid), PaletteControlCallBack,
                                         API_PalEnabled_FloorPlan + API_PalEnabled_Section + API_PalEnabled_Elevation +
                                             API_PalEnabled_InteriorElevation + API_PalEnabled_3D +
                                             API_PalEnabled_Detail + API_PalEnabled_Worksheet + API_PalEnabled_Layout +
                                             API_PalEnabled_DocumentFrom3D,
                                         GSGuid2APIGuid (paletteGuid));
}

GSErrCode ControlPalette::UnregisterPaletteControlCallBack ()
{
    return ACAPI_UnregisterModelessWindow (GS::CalculateHashValue (paletteGuid));
}
