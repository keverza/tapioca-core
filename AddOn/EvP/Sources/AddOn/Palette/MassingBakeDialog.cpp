#include "APIEnvir.h"
#include "ACAPinc.h"
#include "Palette/MassingBakeDialog.hpp"
#include "Palette/AttributePickerTypes.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "ResourceIds.hpp"
#include "DGModule.hpp"
#include "DGFileDialog.hpp"
#include "FileTypeManager.hpp"
#include "Location.hpp"

namespace evp::massingbakeui {
using Kind = geomsrv::archviz::massingbake::Kind;
namespace {
namespace js = evp::nodegraph::json;
using V = js::JsonValue;
struct Attribute {
    std::string key;
    API_AttrTypeID type = API_ZombieAttrID;
    std::unique_ptr<DG::PushCheck> host;
    GS::Owner<API_AttributePicker> picker; // Destroy before the DG host.
};
class Dialog final : public DG::ModalDialog,
                     public DG::PanelObserver,
                     public DG::ButtonItemObserver,
                     public DG::CheckItemObserver {
  public:
    explicit Dialog (Kind kind)
        : DG::ModalDialog (ACAPI_GetOwnResModule (), MassingBakeResId, ACAPI_GetOwnResModule ()), kind (kind),
          bake (GetReference (), 1), cancel (GetReference (), 2), status (GetReference (), 3),
          contourPen (GetReference (), 4), fillPen (GetReference (), 5), export2D (GetReference (), 6)
    {
        Attach (*this);
        bake.Attach (*this);
        cancel.Attach (*this);
        export2D.Attach (*this);
        if (kind != Kind::Slices)
            export2D.Hide ();
        SetTitle (kind == Kind::Slices     ? "Bake / export massing story slices"
                  : kind == Kind::Envelope ? "Bake massing envelope"
                                           : "Bake collapse zone");
        status.SetText (kind == Kind::Slices
                            ? "Bake: one Undo step. Export 2D: JSON contours only, no model changes."
                            : "Creates copies. Envelope: two Undo steps; fills: one. No write retries.");
        contourPen.Hide ();
        fillPen.Hide ();
        API_Element defaults {};
        defaults.header.type = kind == Kind::Slices ? API_SlabID : kind == Kind::Envelope ? API_MorphID : API_HatchID;
        valid = ACAPI_Element_GetDefaults (&defaults, nullptr) == NoError;
        AddAttribute ("layer", "Layer", "Layer", defaults.header.layer);
        if (kind == Kind::Slices) {
            AddCheck (composite, "Use slab composite", false);
            AddAttribute ("material", "Slab building material (Basic)", "BuildingMaterial",
                          defaults.slab.buildingMaterial);
            AddAttribute ("composite", "Slab composite (when enabled)", "SlabComposite", defaults.slab.composite);
            AddLength (slabThickness, "Basic slab thickness",
                       defaults.slab.thickness > 0 ? defaults.slab.thickness : 0.2);
            AddCheck (walls, "Create perimeter walls (including courtyards)", false);
            API_Element wall {};
            wall.header.type = API_WallID;
            valid = valid && ACAPI_Element_GetDefaults (&wall, nullptr) == NoError;
            AddAttribute ("wallLayer", "Wall layer", "Layer", wall.header.layer);
            AddCheck (wallComposite, "Use wall composite", false);
            AddAttribute ("wallMaterial", "Wall building material (Basic)", "BuildingMaterial",
                          wall.wall.buildingMaterial);
            AddAttribute ("wallComposite", "Wall composite (when enabled)", "WallComposite", wall.wall.composite);
            AddLength (wallThickness, "Basic wall thickness", wall.wall.thickness > 0 ? wall.wall.thickness : 0.2);
            Note ("Counted contours; slab top at floor. Walls use nominal storey height, not roof trimming.");
        }
        else if (kind == Kind::Envelope) {
            AddAttribute ("material", "Morph building material", "BuildingMaterial", defaults.morph.buildingMaterial);
            AddAttribute ("surface", "Morph surface", "Surface", ACAPI_CreateAttributeIndex (1));
            Note ("One solid Morph per parcel envelope. Current slopes and holes are retained.");
        }
        else {
            AddAttribute ("fill", "Fill pattern", "Fill", defaults.hatch.fillInd);
            AddAttribute ("line", "Contour line type", "LineType", defaults.hatch.ltypeInd);
            Note ("Contour pen / hatch pen (native project pen pickers below). Background is transparent.");
            contourPen.SetValue (defaults.hatch.contPen.penIndex > 0 ? defaults.hatch.contPen.penIndex : 1);
            fillPen.SetValue (defaults.hatch.fillPen.penIndex > 0 ? defaults.hatch.fillPen.penIndex : 1);
            contourPen.Show ();
            fillPen.Show ();
            Note ("Plan fill of the complete zone, not a flattened terrain triangle mesh. Active home storey.");
        }
        if (!valid) {
            bake.Disable ();
            status.SetText ("A required Archicad native picker/default could not be loaded. Nothing will be baked.");
        }
        EnableSettings ();
    }
    ~Dialog ()
    {
        for (auto& row : attributes)
            row.host->Detach (*this);
        for (auto* check : { composite.get (), walls.get (), wallComposite.get () })
            if (check)
                check->Detach (*this);
        bake.Detach (*this);
        cancel.Detach (*this);
        export2D.Detach (*this);
        Detach (*this);
    }
    V Settings () const
    {
        return settings;
    }
    Action Choice () const
    {
        return action;
    }

  private:
    void Note (const char* text)
    {
        auto label = std::make_unique<DG::LeftText> (*this, DG::Rect (12, top, 508, short (top + 22)));
        label->SetText (GS::UniString (text, CC_UTF8));
        label->Show ();
        labels.push_back (std::move (label));
        top += 30;
    }
    DG::Rect Field (const char* text)
    {
        auto label = std::make_unique<DG::LeftText> (*this, DG::Rect (12, top, 250, short (top + 23)));
        label->SetText (GS::UniString (text, CC_UTF8));
        label->Show ();
        labels.push_back (std::move (label));
        const DG::Rect rect (256, top, 508, short (top + 23));
        top += 30;
        return rect;
    }
    void AddCheck (std::unique_ptr<DG::CheckBox>& check, const char* text, bool checked)
    {
        check = std::make_unique<DG::CheckBox> (*this, DG::Rect (12, top, 508, short (top + 23)));
        check->SetText (GS::UniString (text, CC_UTF8));
        check->SetState (checked);
        check->Attach (*this);
        check->Show ();
        top += 30;
    }
    void AddLength (std::unique_ptr<DG::LengthEdit>& edit, const char* text, double seed)
    {
        edit = std::make_unique<DG::LengthEdit> (*this, Field (text));
        edit->SetMin (0.001);
        edit->SetMax (10);
        edit->SetValue (seed);
        edit->Show ();
    }
    void AddAttribute (const char* key, const char* label, const char* type, API_AttributeIndex seed)
    {
        Attribute row;
        row.key = key;
        row.host = std::make_unique<DG::PushCheck> (*this, Field (label));
        API_UserControlType controlType;
        if (!evp::UserControlTypeFor (type, controlType, row.type)) {
            valid = false;
            return;
        }
        API_AttributePickerParams params;
        params.type = controlType;
        params.dialogID = GetId ();
        params.itemID = row.host->GetId ();
        params.pushCheckAppearance = API_AttributePickerParams::PushCheckAppearance::ArrowIconAndText;
        if (ACAPI_Dialog_CreateAttributePicker (params, row.picker) != NoError || row.picker == nullptr) {
            valid = false;
            return;
        }
        row.picker->SetSelectedAttributeIndex (seed);
        row.host->Attach (*this);
        row.host->Show ();
        attributes.push_back (std::move (row));
    }
    void ButtonClicked (const DG::ButtonClickEvent& event) override
    {
        if (event.GetSource () == &cancel) {
            PostCloseRequest (Cancel);
            return;
        }
        if (event.GetSource () == &export2D && kind == Kind::Slices) {
            // Export has no construction settings and must work even when a
            // project attribute picker/default is unavailable for baking.
            action = Action::Export2D;
            PostCloseRequest (Accept);
            return;
        }
        if (event.GetSource () != &bake || !valid)
            return;
        js::JsonObject values;
        const bool isComposite = composite && composite->IsChecked ();
        const bool withWalls = walls && walls->IsChecked ();
        const bool isWallComposite = wallComposite && wallComposite->IsChecked ();
        for (const auto& row : attributes) {
            if ((row.key == "material" && isComposite) || (row.key == "composite" && !isComposite) ||
                (row.key.find ("wall") == 0 && !withWalls) || (row.key == "wallMaterial" && isWallComposite) ||
                (row.key == "wallComposite" && !isWallComposite))
                continue;
            API_Attribute attribute {};
            attribute.header.typeID = row.type;
            attribute.header.index = row.picker->GetSelectedAttributeIndex ();
            if (ACAPI_Attribute_Get (&attribute) != NoError) {
                status.SetText ("Choose an existing project attribute for every enabled setting.");
                return;
            }
            values[row.key] =
                V::Object ({ { "index", V::Integer (attribute.header.index.ToInt32_Deprecated ()) },
                             { "guid", V::String (APIGuidToString (attribute.header.guid).ToCStr ().Get ()) },
                             { "name", V::String (geomsrv::AttributeIndexToName (row.type, attribute.header.index)
                                                      .ToCStr (0, MaxUSize, CC_UTF8)
                                                      .Get ()) } });
            if (row.key == "wallComposite")
                values["wallCompositeThickness"] = V::Double (attribute.compWall.totalThick);
        }
        values["compositeOn"] = V::Bool (isComposite);
        values["walls"] = V::Bool (withWalls);
        values["wallCompositeOn"] = V::Bool (isWallComposite);
        if (slabThickness)
            values["slabThickness"] = V::Double (slabThickness->GetValue ());
        if (wallThickness)
            values["wallThickness"] = withWalls && isWallComposite ? values.at ("wallCompositeThickness")
                                                                   : V::Double (wallThickness->GetValue ());
        if (kind == Kind::Collapse) {
            values["pen"] = V::Integer (contourPen.GetValue ());
            values["fillPen"] = V::Integer (fillPen.GetValue ());
        }
        settings = V::Object (std::move (values));
        action = Action::Bake;
        PostCloseRequest (Accept);
    }
    void EnableSettings ()
    {
        const bool useComposite = composite && composite->IsChecked ();
        const bool withWalls = walls && walls->IsChecked ();
        const bool useWallComposite = wallComposite && wallComposite->IsChecked ();
        for (auto& row : attributes) {
            const bool enabled =
                !((row.key == "material" && useComposite) || (row.key == "composite" && !useComposite) ||
                  (row.key.find ("wall") == 0 && !withWalls) || (row.key == "wallMaterial" && useWallComposite) ||
                  (row.key == "wallComposite" && !useWallComposite));
            if (enabled)
                row.host->Enable ();
            else
                row.host->Disable ();
        }
        if (slabThickness) {
            if (useComposite)
                slabThickness->Disable ();
            else
                slabThickness->Enable ();
        }
        if (wallThickness) {
            if (!withWalls || useWallComposite)
                wallThickness->Disable ();
            else
                wallThickness->Enable ();
        }
        if (wallComposite) {
            if (withWalls)
                wallComposite->Enable ();
            else
                wallComposite->Disable ();
        }
    }
    void CheckItemChanged (const DG::CheckItemChangeEvent& event) override
    {
        for (auto& row : attributes)
            if (event.GetSource () == row.host.get ()) {
                row.picker->Invoke ();
                row.host->Uncheck ();
                return;
            }
        EnableSettings ();
    }
    Kind kind;
    DG::Button bake, cancel;
    DG::LeftText status;
    DG::UserControl contourPen, fillPen;
    DG::Button export2D;
    Action action = Action::Cancel;
    short top = 12;
    bool valid = true;
    std::vector<std::unique_ptr<DG::LeftText>> labels;
    std::vector<Attribute> attributes;
    std::unique_ptr<DG::CheckBox> composite, walls, wallComposite;
    std::unique_ptr<DG::LengthEdit> slabThickness, wallThickness;
    V settings;
};
} // namespace
Action AskSettings (Kind kind, V& settings)
{
    Dialog dialog (kind);
    if (!dialog.Invoke ())
        return Action::Cancel;
    settings = dialog.Settings ();
    return dialog.Choice ();
}
bool AskExportPath (GS::UniString& path)
{
    FTM::FileTypeManager manager ("Tapioca.StorySliceContours");
    const auto root = manager.AddGroup ("Story slice 2D contours");
    const FTM::FileType fileType ("Story slice contours (*.json)", "json", 0, 0, 0);
    const auto type = manager.AddType (fileType, root);
    DG::FileDialog dialog (DG::FileDialog::Save);
    if (type != FTM::UnknownType)
        dialog.AddFilter (type);
    dialog.SetFilterRoot (root);
    IO::Location folder;
    GS::UniString directory;
    if (dialog.GetFolder (&folder) && folder.ToPath (&directory) == NoError) {
        const GS::UniString suggested (directory + GS::UniString ("\\story-slices-2d.json"));
        dialog.SelectFile (IO::Location (suggested), false);
    }
    return dialog.Invoke () && dialog.GetSelectedFile ().ToPath (&path) == NoError;
}
} // namespace evp::massingbakeui
