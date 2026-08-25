#include "Palette/PreviewPanel.hpp"

#include "PlanOverlay/OverlayWindow.hpp"

#include "DGWin.h"

#include <windows.h>

#include <algorithm>

namespace evp {

bool PreviewPanel::EmbeddedInputAvailable () const
{
    return IsEnabled () && paletteVisible && kind == "3d" && canvas && host.current == previewpanel::Host::Band &&
           !host.transition && !collapsed;
}

bool PreviewPanel::PlanInputAvailable () const
{
    return IsEnabled () && paletteVisible && kind == "plan2d" && canvas && !collapsed;
}

void PreviewPanel::FitSelectedPlanFrame ()
{
    geomsrv::annotation::Point3 minimum;
    geomsrv::annotation::Point3 maximum;
    const geomsrv::annotation::Frame* const frame = drawList.SelectedFrame ();
    if (frame == nullptr || !geomsrv::annotation::GetBounds (*frame, minimum, maximum)) {
        planCamera.SetBounds ({});
        planFitPending = false;
        return;
    }
    planCamera.SetBounds ({ minimum.x, minimum.y, maximum.x, maximum.y, true });
    planFitPending = !planCamera.Fit ();
}

void PreviewPanel::UpdatePlanViewport ()
{
    if (kind != "plan2d" || !canvas)
        return;
    RECT client {};
    HWND const hwnd = static_cast<HWND> (CanvasWindow ());
    if (hwnd != nullptr && ::GetClientRect (hwnd, &client) && client.right > client.left && client.bottom > client.top)
        planCamera.SetViewport (client.right - client.left, client.bottom - client.top);
    else
        planCamera.SetViewport (canvas->GetWidth (), canvas->GetHeight ());
    if (planFitPending && planCamera.Fit ())
        planFitPending = false;
}

void PreviewPanel::UpdateOpacity ()
{
    if (!opacitySlider)
        return;
    overlayOpacityPercent = std::clamp ((int) opacitySlider->GetValue (), 0, 100);
    opacityLabel->SetText (GS::UniString::Printf ("Overlay opacity %d%%", overlayOpacityPercent));
    if (overlayActive && overlaySession.ownership.ownsWindow)
        geomsrv::planoverlay::SetOpacity (geomsrv::planoverlay::Owner::Watch, (overlayOpacityPercent * 255 + 50) / 100);
}

void* PreviewPanel::CanvasWindow () const
{
    if (!canvas)
        return nullptr;
    HWND const hwnd = DGGetDialogItemWindow (panel.GetId (), canvas->GetId ());
    return hwnd != nullptr && ::IsWindow (hwnd) ? hwnd : nullptr;
}

} // namespace evp
