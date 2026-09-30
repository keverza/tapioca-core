#include "ParamColorControls.hpp"
#include "ParamPanel.hpp"
#include "ParamValues.hpp"
#include "DGNativeContexts.hpp"
#include "AppearanceManager/DGColorCatalog.hpp"

namespace evp {

void BuildColorControl (ParamControl& pc, const GS::ObjectState& metadata, const DG::Panel& panel, const DG::Rect& seed,
                        DG::ButtonItemObserver& buttonObserver, DG::UserItemObserver& swatchObserver)
{
    pc.kind = ParamControl::Kind::Color;
    GS::UniString value;
    metadata.Get ("default", value);
    Gfx::Color parsed;
    if (HexToColor (value, parsed))
        pc.colorHex = ColorToHex (parsed);
    auto button = std::make_unique<DG::Button> (panel, seed);
    button->SetText (pc.colorHex.IsEmpty () ? GS::UniString ("Choose colour...") : pc.colorHex);
    button->Attach (buttonObserver);
    pc.control = std::move (button);
    pc.colorSwatch = std::make_unique<DG::UserItem> (panel, seed, DG::UserItem::Normal, DG::UserItem::ClientFrame);
    pc.colorSwatch->Attach (swatchObserver);
}

bool OpenColorChooser (DG::Button& button, DG::UserItem* swatch, GS::UniString& hex)
{
    // Modal: invoked directly from the palette's main-thread button handler.
    Gfx::Color color;
    if (!hex.IsEmpty ())
        HexToColor (hex, color);
    if (!DG::GetColor ("Choose colour", &color))
        return false;
    hex = ColorToHex (color);
    button.SetText (hex);
    if (swatch != nullptr)
        swatch->Redraw ();
    return true;
}

bool DrawColorSwatch (const DG::UserItemUpdateEvent& ev, const DG::UserItem* swatch, const GS::UniString& hex)
{
    if (swatch == nullptr || ev.GetSource () != swatch)
        return false;
    Gfx::Color color = DG::ColorCatalog::GetColor (DG::ColorId::StandardTextBackgroundColor);
    HexToColor (hex, color);
    NewDisplay::UserItemUpdateNativeContext context (ev);
    context.FillRect (0.0f, 0.0f, (float) swatch->GetClientWidth (), (float) swatch->GetClientHeight (),
                      color.GetRed (), color.GetGreen (), color.GetBlue ());
    return true;
}

bool ParamPanel::HandleColorSwatchUpdate (const DG::UserItemUpdateEvent& ev) const
{
    for (const ParamControl& pc : paramControls) {
        if (DrawColorSwatch (ev, pc.colorSwatch.get (), pc.colorHex))
            return true;
    }
    return false;
}

} // namespace evp
