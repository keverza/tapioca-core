#include "APIEnvir.h"
#include "ACAPinc.h"
#include "Palette/HardscapeMaterialDialog.hpp"
#include "Palette/AttributePickerTypes.hpp"
#include "NativeCommands/HardscapeMaterial.hpp"
#include "ResourceIds.hpp"
#include "DGModule.hpp"
#include <algorithm>
#include <memory>

namespace tapioca::materialui {
namespace {
namespace lg = geometry;
struct Row {
    lg::MaterialAttributeKind kind;
    std::unique_ptr<DG::CheckBox> use;
    std::unique_ptr<DG::PushCheck> host;
    GS::Owner<API_AttributePicker> picker; // Destroy before the DG host.
};
class Dialog final : public DG::ModalDialog,
                     public DG::PanelObserver,
                     public DG::ButtonItemObserver,
                     public DG::CheckItemObserver {
  public:
    Dialog (const lg::MaterialSpec& material, const lg::MaterialCatalog& catalog)
        : DG::ModalDialog (ACAPI_GetOwnResModule (), HardscapeMaterialResId, ACAPI_GetOwnResModule ()),
          apply (GetReference (), 1), cancel (GetReference (), 2), status (GetReference (), 3), value (material)
    {
        Attach (*this);
        apply.Attach (*this);
        cancel.Attach (*this);
        Add (lg::MaterialAttributeKind::Fill, "Use fill", "Fill", material.fillGuid, material.fillName, catalog, 16);
        Add (lg::MaterialAttributeKind::Surface, "Use surface", "Surface", material.surfaceGuid, material.surfaceName,
             catalog, 54);
        std::string error;
        const bool resolved = lg::ValidateMaterialAvailability (material, catalog, error);
        status.SetText (!valid ? "A native attribute picker could not be opened. No changes will be applied."
                        : resolved
                            ? "Unchecked = explicitly unassigned. Apply stores GUID and name, not index."
                            : "A saved material is missing/unconfirmed. Apply explicitly replaces enabled choices.");
        if (!valid)
            apply.Disable ();
    }
    ~Dialog ()
    {
        for (auto& row : rows) {
            row.host->Detach (*this);
            row.use->Detach (*this);
        }
        apply.Detach (*this);
        cancel.Detach (*this);
        Detach (*this);
    }
    lg::MaterialSpec Value () const
    {
        return value;
    }

  private:
    void Add (lg::MaterialAttributeKind kind, const char* text, const char* type, const std::string& guid,
              const std::string& name, const lg::MaterialCatalog& catalog, short y)
    {
        Row row { kind };
        row.use = std::make_unique<DG::CheckBox> (*this, DG::Rect (12, y, 132, short (y + 23)));
        row.use->SetText (text);
        row.use->SetState (!guid.empty () || !name.empty ());
        row.use->Attach (*this);
        row.use->Show ();
        row.host = std::make_unique<DG::PushCheck> (*this, DG::Rect (140, y, 508, short (y + 23)));
        row.host->Attach (*this);
        row.host->Show ();
        const lg::MaterialAttribute* seed = nullptr;
        for (const auto& attribute : catalog.attributes)
            if (attribute.kind == kind && (!seed || attribute.guid == guid))
                seed = &attribute;
        if (seed) {
            API_UserControlType control;
            API_AttrTypeID attributeType;
            if (!evp::UserControlTypeFor (type, control, attributeType))
                valid = false;
            else {
                API_AttributePickerParams params;
                params.type = control;
                params.dialogID = GetId ();
                params.itemID = row.host->GetId ();
                params.pushCheckAppearance = API_AttributePickerParams::PushCheckAppearance::ArrowIconAndText;
                if (ACAPI_Dialog_CreateAttributePicker (params, row.picker) != NoError || row.picker == nullptr)
                    valid = false;
                else
                    row.picker->SetSelectedAttributeIndex (ACAPI_CreateAttributeIndex (seed->index));
            }
        }
        else
            row.host->Disable ();
        rows.push_back (std::move (row));
    }
    void CheckItemChanged (const DG::CheckItemChangeEvent& event) override
    {
        for (auto& row : rows)
            if (event.GetSource () == row.host.get () && row.picker != nullptr) {
                if (row.picker->Invoke ())
                    row.use->SetState (true);
                row.host->Uncheck ();
                return;
            }
    }
    void ButtonClicked (const DG::ButtonClickEvent& event) override
    {
        if (event.GetSource () == &cancel) {
            PostCloseRequest (Cancel);
            return;
        }
        if (event.GetSource () != &apply || !valid)
            return;
        lg::MaterialCatalog current;
        std::string error;
        if (!ReadCatalog (current, error)) {
            status.SetText (GS::UniString (error.c_str (), CC_UTF8));
            return;
        }
        auto candidate = value;
        for (const auto& row : rows) {
            if (!row.use->IsChecked ()) {
                if (row.kind == lg::MaterialAttributeKind::Fill) {
                    candidate.fillGuid.clear ();
                    candidate.fillName.clear ();
                }
                else {
                    candidate.surfaceGuid.clear ();
                    candidate.surfaceName.clear ();
                }
                continue;
            }
            if (row.picker == nullptr) {
                status.SetText ("No attribute is available; uncheck it to explicitly clear it.");
                return;
            }
            const auto index = row.picker->GetSelectedAttributeIndex ().ToInt32_Deprecated ();
            const auto attribute =
                std::find_if (current.attributes.begin (), current.attributes.end (),
                              [&] (const auto& a) { return a.kind == row.kind && a.index == index; });
            if (attribute == current.attributes.end () || !lg::PickMaterialAttribute (candidate, *attribute, error)) {
                status.SetText ("The project attributes changed. Choose an existing attribute again.");
                return;
            }
        }
        if (!lg::ValidateMaterialAvailability (candidate, current, error)) {
            status.SetText (GS::UniString (error.c_str (), CC_UTF8));
            return;
        }
        value = std::move (candidate);
        PostCloseRequest (Accept);
    }
    DG::Button apply, cancel;
    DG::LeftText status;
    lg::MaterialSpec value;
    std::vector<Row> rows;
    bool valid = true;
};
} // namespace
bool Pick (const lg::MaterialSpec& before, const lg::MaterialCatalog& catalog, lg::MaterialSpec& picked,
           std::string& error)
{
    error.clear ();
    Dialog dialog (before, catalog);
    if (!dialog.Invoke ())
        return false;
    picked = dialog.Value ();
    return true;
}
} // namespace tapioca::materialui
