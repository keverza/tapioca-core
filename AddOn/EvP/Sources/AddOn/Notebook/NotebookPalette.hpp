#ifndef EVP_NOTEBOOK_NOTEBOOKPALETTE_HPP
#define EVP_NOTEBOOK_NOTEBOOKPALETTE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGBrowser.hpp"
#include "DGModule.hpp"

class NotebookPalette final : public DG::Palette,
                              public DG::PanelObserver {
public:
    static bool             HasInstance ();
    static void             CreateInstance ();
    static NotebookPalette& GetInstance ();
    static void             DestroyInstance ();

    static void Open ();

    static GSErrCode RegisterPaletteControlCallBack ();
    static GSErrCode UnregisterPaletteControlCallBack ();

    ~NotebookPalette () override;

private:
    NotebookPalette ();

    void Show ();
    void Hide ();
    void LoadNotebookPage ();

    void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;
    void PanelResized (const DG::PanelResizeEvent& ev) override;

    static GSErrCode PaletteControlCallBack (Int32 referenceId,
                                             API_PaletteMessageID messageId,
                                             GS::IntPtr param);

    static const GS::Guid           paletteGuid;
    static GS::Ref<NotebookPalette> instance;

    DG::Browser browser;
};

#endif
