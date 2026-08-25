// Automatic selection-preview policy for the ControlPalette shell. Kept out of
// ControlPaletteRun.cpp because this is event coalescing around a normal run, not a
// second runner.

#include "ControlPalette.hpp"

#include "Preview/PreviewRuntimeState.hpp"
#include "Python/RunCancel.hpp"

void ControlPalette::CancelAutomaticPreview (bool cancelInFlight)
{
    automaticPreview.Cancel ();
    if (cancelInFlight && automaticPreviewGeneration != 0 && runActive.load () &&
        runGeneration == automaticPreviewGeneration) {
        automaticPreviewFolder.Clear ();
        evp::RunCancel::Get ().Request (evp::CancelReason::StopButton);
    }
}

bool ControlPalette::AutomaticPreviewEligible () const
{
    const evp::CommandInfo* const info = SelectedCommand ();
    return IsVisible () && info != nullptr && info->previewOnSelection && !info->selectionSets.IsEmpty () &&
           selectionSets.AllRolesNonEmpty () && evp::preview::PreviewRuntimeState::Get ().IsEnabled () &&
           params.WhatIsMissing ().IsEmpty ();
}

void ControlPalette::PollAutomaticPreview ()
{
    if (automaticPreview.ShouldLaunch (evp::AutomaticPreviewState::Clock::now (), AutomaticPreviewEligible (),
                                       runActive.load ()))
        RunSelected (GS::UniString (), GS::UniString (), true);

    // Publish precedes worker completion. Ownership must be checked first.
    if (!runActive.load () && preview.PollRetained ()) {
        Layout ();
        Redraw ();
    }
}
