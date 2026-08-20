#ifndef EVP_ABOUTDIALOG_HPP
#define EVP_ABOUTDIALOG_HPP

// F6 (palette-ui-plan.md) — the second item under the add-on menu.
//
// Shape copied from the vendored Tapir add-on's AboutDialog: a DG::ModalDialog
// built from a .grc, one OK button and auxiliary folder/update buttons. It lives under
// Palette/ with the other DG-owned UI surfaces, but shares no state or events with
// the main command palette.
//
// The compact product identity dialog also links directly to the command and log
// folders and the future update endpoint.
//
// Constructed and Invoke()d from the menu handler, i.e. on the main thread, so it
// may read ACAPI directly — no MainThreadGate hop.

#include "DGModule.hpp"

class AboutDialog : public DG::ModalDialog,
                    public DG::PanelObserver,
                    public DG::ButtonItemObserver,
                    public DG::CompoundItemObserver {
  public:
    AboutDialog ();
    ~AboutDialog ();

  private:
    virtual void ButtonClicked (const DG::ButtonClickEvent& ev) override;

    // Opens `path` in Explorer, creating it first so the button is never a no-op
    // on a fresh install (the logs folder does not exist until something logs).
    static void RevealFolder (const GS::UniString& path);

    DG::Button okButton;
    DG::Button logsButton;
    DG::Button commandsButton;
    DG::Button updatesButton;
    DG::CenterText versionText;
};

#endif
