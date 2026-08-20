#include "Palette/SelectionSetPanel.hpp"
#include "AddOnCommands.hpp"
#include "NativeCommands/SelectionSetStore.hpp"
#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"
#include "Python/PathUtils.hpp"

namespace evp {

using namespace evp::palette;

SelectionSetPanel::SelectionSetPanel (const DG::Panel& panel_, DG::ButtonItemObserver& observer_) :
    panel (panel_), observer (observer_)
{
}

void SelectionSetPanel::Clear ()
{
    rows.clear ();
    geomsrv::SelectionSetStore::Get ().Clear ();
}

void SelectionSetPanel::Rebuild (const GS::Array<GS::UniString>& names)
{
    Clear ();
    geomsrv::SelectionSetStore::Get ().Configure (names);
    for (const GS::UniString& name : names) {
        Row row;
        row.name = name;
        const DG::Rect seed (Margin, 0, Margin + 50, RowHeight);
        row.label = std::make_unique<DG::LeftText> (panel, seed);
        row.update = std::make_unique<DG::Button> (panel, seed);
        row.add = std::make_unique<DG::Button> (panel, seed);
        row.remove = std::make_unique<DG::Button> (panel, seed);
        row.reselect = std::make_unique<DG::Button> (panel, seed);
        row.clear = std::make_unique<DG::Button> (panel, seed);
        row.update->SetText ("Update");
        row.add->SetText ("Add");
        row.remove->SetText ("Remove");
        row.reselect->SetText ("Reselect");
        row.clear->SetText ("Clear");
        row.update->Attach (observer);
        row.add->Attach (observer);
        row.remove->Attach (observer);
        row.reselect->Attach (observer);
        row.clear->Attach (observer);
        RefreshLabel (row);
        rows.push_back (std::move (row));
    }
}

void SelectionSetPanel::RefreshLabel (Row& row)
{
    const GS::Int32 count = (GS::Int32) geomsrv::SelectionSetStore::Get ().Values (row.name).GetSize ();
    // "<role> (<count>)" — see the header for why. Refreshed on every PlaceAt.
    row.label->SetText (GS::UniString::Printf ("%T (%d)", row.name.ToPrintf (), (int) count));
}

short SelectionSetPanel::PlaceAt (short top, short left, short right, const PaletteScroll& clip)
{
    short y = top;
    for (Row& row : rows) {
        RefreshLabel (row);
        const bool titleVisible = clip.IsVisible (y, (short) (y + 16));
        clip.Place (row.label.get (), DG::Rect (left, y, right, (short) (y + 16)));
        y += 18;
        constexpr short gap = 4;
        const short width = (short) ((right - left - 4 * gap) / 5);
        DG::ButtonItem* buttons[] = { row.update.get (), row.add.get (), row.remove.get (), row.reselect.get (), row.clear.get () };
        for (short i = 0; i < 5; ++i) {
            const short x = (short) (left + i * (width + gap));
            clip.Place (buttons[i], DG::Rect (x, y, (short) (x + width), (short) (y + RowHeight)));
            if (titleVisible && clip.IsVisible (y, (short) (y + RowHeight))) buttons[i]->Show ();
            else buttons[i]->Hide ();
        }
        y += RowHeight + 2;
        if (titleVisible) row.label->Show (); else row.label->Hide ();
    }
    return rows.empty () ? 0 : (short) (y - top + 4);
}

bool SelectionSetPanel::Apply (Row& row, Action action)
{
    GS::ObjectState params;
    params.Add ("name", row.name);
    GS::String command;
    const char* verb = "";
    if (action == Action::Reselect) {
        command = "ReselectSelectionSet";
        verb = "Reselected";
    } else {
        command = "ModifySelectionSet";
        const char* op = action == Action::Update ? "update" : action == Action::Add ? "add"
                       : action == Action::Remove ? "remove" : "clear";
        params.Add ("op", GS::UniString (op));
        if (action != Action::Clear)
            params.Add ("current", true);
        verb = action == Action::Update ? "Updated" : action == Action::Add ? "Added"
             : action == Action::Remove ? "Removed" : "Cleared";
    }
    const geomsrv::NativeCommandResult result = geomsrv::ExecuteNativeCommand (command, params);
    if (!result.ok) {
        AppendTextLine (ScanLogPath (), "selection-set panel: " + result.error);
        row.lastAction = "Operation failed - see logs\\scan.log";
        return true;
    }
    const GS::ObjectState& response = result.data;
    GS::Int32 count = 0;
    GS::Int32 changed = 0;
    response.Get ("count", count);
    response.Get ("changed", changed);
    GS::Array<GS::UniString> missing;
    response.Get ("missing", missing);
    row.lastAction = GS::UniString::Printf ("%s: %d this batch, %d saved",
                                             verb, (int) changed, (int) count);
    if (!missing.IsEmpty ())
        row.lastAction += GS::UniString::Printf (", %d missing", (int) missing.GetSize ());
    return true;
}

bool SelectionSetPanel::HandleButtonClicked (const DG::ButtonClickEvent& ev)
{
    for (Row& row : rows) {
        if (ev.GetSource () == row.update.get ()) return Apply (row, Action::Update);
        if (ev.GetSource () == row.add.get ()) return Apply (row, Action::Add);
        if (ev.GetSource () == row.remove.get ()) return Apply (row, Action::Remove);
        if (ev.GetSource () == row.reselect.get ()) return Apply (row, Action::Reselect);
        if (ev.GetSource () == row.clear.get ()) return Apply (row, Action::Clear);
    }
    return false;
}

} // namespace evp
