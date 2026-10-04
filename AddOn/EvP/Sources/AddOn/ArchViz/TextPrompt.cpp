// ArchViz/TextPrompt -- see the header.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ArchViz/TextPrompt.hpp"

#include "ResourceIds.hpp"

#include "DGModule.hpp"

namespace geomsrv {
namespace archviz {
namespace textprompt {

namespace {

class Dialog : public DG::ModalDialog, public DG::PanelObserver, public DG::ButtonItemObserver {
  public:
    Dialog (const GS::UniString& prompt, const GS::UniString& text)
        : DG::ModalDialog (ACAPI_GetOwnResModule (), TextPromptResId, ACAPI_GetOwnResModule ()),
          ok (GetReference (), TextPromptOkButtonId), cancel (GetReference (), TextPromptCancelButtonId),
          label (GetReference (), TextPromptLabelId), edit (GetReference (), TextPromptEditId)
    {
        Attach (*this);
        ok.Attach (*this);
        cancel.Attach (*this);
        label.SetText (prompt);
        edit.SetText (text);
        edit.SelectAll ();
    }

    ~Dialog ()
    {
        ok.Detach (*this);
        cancel.Detach (*this);
        Detach (*this);
    }

    GS::UniString Answer () const
    {
        return answer;
    }

  private:
    void ButtonClicked (const DG::ButtonClickEvent& ev) override
    {
        if (ev.GetSource () == &ok) {
            answer = edit.GetText ();
            PostCloseRequest (Accept);
        }
        else if (ev.GetSource () == &cancel) {
            PostCloseRequest (Cancel);
        }
    }

    DG::Button ok;
    DG::Button cancel;
    DG::LeftText label;
    DG::TextEdit edit;
    GS::UniString answer;
};

} // namespace

bool Ask (const std::string& prompt, const std::string& text, std::string& answer)
{
    Dialog dialog (GS::UniString (prompt.c_str (), CC_UTF8), GS::UniString (text.c_str (), CC_UTF8));
    if (!dialog.Invoke ())
        return false;
    answer = dialog.Answer ().ToCStr (0, MaxUSize, CC_UTF8).Get ();
    return true;
}

} // namespace textprompt
} // namespace archviz
} // namespace geomsrv
