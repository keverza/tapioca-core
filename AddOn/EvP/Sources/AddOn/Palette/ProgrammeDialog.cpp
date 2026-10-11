#include "APIEnvir.h"
#include "ACAPinc.h"
#include "Palette/ProgrammeDialog.hpp"
#include "ResourceIds.hpp"
#include "DGModule.hpp"
#include "Color.hpp"
#include <array>
#include <memory>

namespace evp::programmeui {
namespace fp = geomsrv::archviz::floorprogramme;
namespace hp = geomsrv::archviz::hudprogramme;
namespace {
class Dialog final : public DG::ModalDialog, public DG::PanelObserver, public DG::ButtonItemObserver {
  public:
    explicit Dialog (const hp::TableEdit& edit)
        : DG::ModalDialog (ACAPI_GetOwnResModule (), ProgrammeDialogResId, ACAPI_GetOwnResModule ()), edit (edit),
          apply (GetReference (), 1), cancel (GetReference (), 2), status (GetReference (), 3)
    {
        Attach (*this);
        apply.Attach (*this);
        cancel.Attach (*this);
        Label ("Type", 12, 30, 12);
        Label ("Rooms", 48, 144, 12);
        Label ("Min net m2", 150, 246, 12);
        Label ("Max net m2", 252, 348, 12);
        Label ("Share %", 354, 508, 12);
        short top = 40;
        for (size_t i = 0; i < edit.before.types.size (); ++i) {
            const auto& type = edit.before.types[i];
            Label (std::to_string (i + 1), 12, 36, top);
            Row row;
            row.fields[0] = Number (48, 144, top, type.rooms, fp::kMinRooms, fp::kMaxRooms);
            row.fields[1] = Number (150, 246, top, type.minM2, fp::kMinArea, fp::kMaxArea);
            row.fields[2] = Number (252, 348, top, type.maxM2, fp::kMinArea, fp::kMaxArea);
            row.fields[3] = Number (354, 508, top, type.share * 100, 0, 100);
            for (size_t field = 0; field < row.fields.size (); ++field)
                row.initial[field] = row.fields[field]->GetValue ();
            rows.push_back (std::move (row));
            top += 30;
        }
        Label ("Rooms use half steps. Net areas are square metres inside walls.", 12, 508, top + 4);
        status.SetRect (DG::Rect (12, top + 30, 508, top + 66));
        status.SetText ("Apply normalises the share total to 100%. Cancel leaves everything unchanged.");
        cancel.SetRect (DG::Rect (300, top + 72, 400, top + 96));
        apply.SetRect (DG::Rect (408, top + 72, 508, top + 96));
        SetClientSize (520, top + 108);
    }
    ~Dialog ()
    {
        apply.Detach (*this);
        cancel.Detach (*this);
        Detach (*this);
    }
    fp::Programme Answer () const
    {
        return answer;
    }

  private:
    struct Row {
        std::array<std::unique_ptr<DG::RealEdit>, 4> fields;
        std::array<double, 4> initial;
    };
    void Label (const std::string& text, short left, short right, short top)
    {
        auto label = std::make_unique<DG::LeftText> (*this, DG::Rect (left, top, right, top + 23));
        label->SetText (GS::UniString (text.c_str (), CC_UTF8));
        label->Show ();
        labels.push_back (std::move (label));
    }
    std::unique_ptr<DG::RealEdit> Number (short left, short right, short top, double value, double low, double high)
    {
        // Plain RealEdit, not AreaEdit: the table contract is m2, independent of project working units.
        auto field = std::make_unique<DG::RealEdit> (*this, DG::Rect (left, top, right, top + 23));
        field->SetMin (low);
        field->SetMax (high);
        field->SetValue (value);
        field->Show ();
        return field;
    }
    void PanelOpened (const DG::PanelOpenEvent&) override
    {
        if (edit.type < rows.size ())
            rows[edit.type].fields[size_t (edit.field)]->SetFocus ();
    }
    void ButtonClicked (const DG::ButtonClickEvent& event) override
    {
        if (event.GetSource () == &cancel) {
            PostCloseRequest (Cancel);
            return;
        }
        if (event.GetSource () != &apply)
            return;
        auto values = edit.before;
        for (size_t i = 0; i < rows.size (); ++i) {
            auto& type = values.types[i];
            // Preserve untouched stored precision, including percentage conversion round-off.
            const std::array<double*, 4> targets { &type.rooms, &type.minM2, &type.maxM2, &type.share };
            for (size_t field = 0; field < targets.size (); ++field) {
                const double number = rows[i].fields[field]->GetValue ();
                if (number != rows[i].initial[field])
                    *targets[field] = field == 3 ? number / 100 : number;
            }
        }
        std::string error;
        if (values == edit.before)
            answer = edit.before; // A no-op Apply never rewrites a valid stored share total.
        else if (!hp::PrepareTable (std::move (values), answer, error)) {
            status.SetText (GS::UniString (error.c_str (), CC_UTF8));
            return;
        }
        PostCloseRequest (Accept);
    }
    hp::TableEdit edit;
    DG::Button apply, cancel;
    DG::LeftText status;
    std::vector<Row> rows;
    std::vector<std::unique_ptr<DG::LeftText>> labels;
    fp::Programme answer;
};
} // namespace

bool Ask (const hp::TableEdit& edit, fp::Programme& answer)
{
    if (!fp::Valid (edit.before) || edit.type >= edit.before.types.size () || size_t (edit.field) > 3)
        return false;
    Dialog dialog (edit);
    if (dialog.Invoke () != DG::ModalDialog::Accept)
        return false;
    answer = dialog.Answer ();
    return true;
}
bool AskColour (const hp::ColourEdit& edit, uint32_t& rgba)
{
    if (!fp::Valid (edit.before) || edit.type >= edit.before.types.size ())
        return false;
    const auto initial = fp::Colour (edit.before.types[edit.type]);
    Gfx::Color colour (UChar (initial >> 24), UChar (initial >> 16), UChar (initial >> 8));
    const std::string title = "Programme: " + fp::Name (edit.before, edit.type);
    if (!DG::GetColor (GS::UniString (title.c_str (), CC_UTF8), &colour))
        return false;
    rgba = uint32_t (colour.GetRed ()) << 24 | uint32_t (colour.GetGreen ()) << 16 | uint32_t (colour.GetBlue ()) << 8 |
           0xFFu;
    return true;
}
} // namespace evp::programmeui
