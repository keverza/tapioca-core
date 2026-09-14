#include "Palette/CameraSetPanel.hpp"

#include "AddOnCommands.hpp"
#include "NativeCommands/CameraSetStore.hpp"
#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"
#include "Python/PathUtils.hpp"

#include <algorithm>

namespace evp {

using namespace evp::palette;

CameraSetPanel::CameraSetPanel (const DG::Panel& panel_, DG::ButtonItemObserver& buttonObserver_,
                                DG::ListBoxObserver& listObserver_)
    : panel (panel_), buttonObserver (buttonObserver_), listObserver (listObserver_)
{
}

void CameraSetPanel::Clear ()
{
    rows.clear ();
    geomsrv::CameraSetStore::Get ().Clear ();
}

void CameraSetPanel::Rebuild (const GS::Array<GS::UniString>& names)
{
    Clear ();
    geomsrv::CameraSetStore::Get ().Configure (names);
    const DG::Rect seed (Margin, 0, Margin + 100, RowHeight);
    for (const GS::UniString& name : names) {
        Row row;
        row.name = name;
        row.label = std::make_unique<DG::LeftText> (panel, seed);
        row.list = std::make_unique<DG::SingleSelListBox> (panel, seed, DG::ListBox::VScroll, DG::ListBox::PartialItems,
                                                           DG::ListBox::NoHeader, 0, DG::ListBox::Frame);
        row.add = std::make_unique<DG::Button> (panel, seed);
        row.update = std::make_unique<DG::Button> (panel, seed);
        row.remove = std::make_unique<DG::Button> (panel, seed);
        row.restore = std::make_unique<DG::Button> (panel, seed);
        row.clear = std::make_unique<DG::Button> (panel, seed);
        row.statusText = std::make_unique<DG::LeftText> (panel, seed);
        row.add->SetText ("Add");
        row.update->SetText ("Update");
        row.remove->SetText ("Remove");
        row.restore->SetText ("Restore");
        row.clear->SetText ("Clear");
        row.list->Attach (listObserver);
        row.add->Attach (buttonObserver);
        row.update->Attach (buttonObserver);
        row.remove->Attach (buttonObserver);
        row.restore->Attach (buttonObserver);
        row.clear->Attach (buttonObserver);
        Populate (row);
        rows.push_back (std::move (row));
    }
}

void CameraSetPanel::Populate (Row& row, short selected)
{
    const std::vector<geomsrv::ArchicadCamera> cameras = geomsrv::CameraSetStore::Get ().Values (row.name);
    row.label->SetText (GS::UniString::Printf ("%T (%d)", row.name.ToPrintf (), (int) cameras.size ()));
    row.list->DeleteItem (DG::ListBox::AllItems);
    row.list->SetTabFieldCount (1);
    for (size_t i = 0; i < cameras.size (); ++i) {
        const geomsrv::ArchicadCamera& camera = cameras[i];
        row.list->AppendItem ();
        const short item = row.list->GetItemCount ();
        row.list->SetTabItemText (item, 1,
                                  GS::UniString::Printf ("%d  (%.1f, %.1f, %.1f)  FOV %.1f", (int) i + 1, camera.eye[0],
                                                         camera.eye[1], camera.eye[2],
                                                         camera.viewConeDegreesHorizontal));
    }
    if (selected > 0 && selected <= row.list->GetItemCount ())
        row.list->SelectItem (selected);
    RefreshActions (row);
}

void CameraSetPanel::RefreshActions (Row& row)
{
    const bool selected = row.list->GetSelectedItem () > 0;
    DG::Button* selectedActions[] = { row.update.get (), row.remove.get (), row.restore.get () };
    for (DG::Button* button : selectedActions) {
        if (selected)
            button->Enable ();
        else
            button->Disable ();
    }
}

short CameraSetPanel::PlaceAt (short top, short left, short right, const PaletteScroll& clip)
{
    short y = top;
    for (Row& row : rows) {
        clip.Place (row.label.get (), DG::Rect (left, y, right, (short) (y + 16)));
        y += 18;
        row.list->SetTabFieldProperties (1, 0, (short) (right - left - 18), DG::ListBox::Left,
                                         DG::ListBox::EndTruncate);
        clip.Place (row.list.get (), DG::Rect (left, y, right, (short) (y + 68)));
        y += 72;
        constexpr short gap = 4;
        const short width = (short) ((right - left - 4 * gap) / 5);
        DG::Button* buttons[] = { row.add.get (), row.update.get (), row.remove.get (), row.restore.get (),
                                  row.clear.get () };
        for (short i = 0; i < 5; ++i) {
            const short x = (short) (left + i * (width + gap));
            clip.Place (buttons[i], DG::Rect (x, y, (short) (x + width), (short) (y + RowHeight)));
        }
        y += RowHeight + 2;
        row.statusText->SetText (row.status);
        clip.Place (row.statusText.get (), DG::Rect (left, y, right, (short) (y + 16)));
        y += 20;
    }
    return rows.empty () ? 0 : (short) (y - top + 4);
}

bool CameraSetPanel::Apply (Row& row, Action action)
{
    GS::ObjectState params;
    params.Add ("name", row.name);
    const char* actionName = action == Action::Add       ? "add"
                             : action == Action::Update  ? "update"
                             : action == Action::Remove  ? "remove"
                             : action == Action::Restore ? "restore"
                                                         : "clear";
    params.Add ("action", GS::UniString (actionName));
    const short selected = row.list->GetSelectedItem ();
    if (action == Action::Update || action == Action::Remove || action == Action::Restore) {
        if (selected < 1)
            return true;
        params.Add ("index", static_cast<GS::Int64> (selected - 1));
    }

    const geomsrv::NativeCommandResult result = geomsrv::ExecuteNativeCommand ("ModifyCameraSet", params);
    if (!result.ok) {
        row.status = result.error;
        AppendTextLine (ScanLogPath (), "camera-set panel: " + result.error);
        return true;
    }
    GS::Int32 count = 0;
    bool threeDWindowInFront = false;
    result.data.Get ("count", count);
    result.data.Get ("threeDWindowInFront", threeDWindowInFront);
    if (action == Action::Restore && count > 0) {
        row.status = threeDWindowInFront ? "Camera and sun restored" : "Camera restored; open the 3D window to see it";
    }
    else
        row.status.Clear ();
    short nextSelection = selected;
    if (action == Action::Add)
        nextSelection = static_cast<short> (count);
    else if (action == Action::Remove)
        nextSelection = static_cast<short> (std::min<GS::Int32> (selected, count));
    else if (action == Action::Clear)
        nextSelection = 0;
    Populate (row, nextSelection);
    return true;
}

bool CameraSetPanel::HandleButtonClicked (const DG::ButtonClickEvent& ev)
{
    for (Row& row : rows) {
        if (ev.GetSource () == row.add.get ())
            return Apply (row, Action::Add);
        if (ev.GetSource () == row.update.get ())
            return Apply (row, Action::Update);
        if (ev.GetSource () == row.remove.get ())
            return Apply (row, Action::Remove);
        if (ev.GetSource () == row.restore.get ())
            return Apply (row, Action::Restore);
        if (ev.GetSource () == row.clear.get ())
            return Apply (row, Action::Clear);
    }
    return false;
}

bool CameraSetPanel::HandleSelectionChanged (const DG::ListBoxSelectionEvent& ev)
{
    for (Row& row : rows) {
        if (ev.GetSource () != row.list.get ())
            continue;
        RefreshActions (row);
        return true;
    }
    return false;
}

} // namespace evp
