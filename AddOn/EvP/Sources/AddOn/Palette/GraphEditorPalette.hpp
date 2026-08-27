#ifndef EVP_PALETTE_GRAPHEDITORPALETTE_HPP
#define EVP_PALETTE_GRAPHEDITORPALETTE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGBrowser.hpp"
#include "DGModule.hpp"

class GraphEditorPalette final : public DG::Palette, public DG::PanelObserver {
  public:
    static bool HasInstance ();
    static void CreateInstance ();
    static GraphEditorPalette& GetInstance ();
    static void DestroyInstance ();

    static void Open ();

    static GSErrCode RegisterPaletteControlCallBack ();
    static GSErrCode UnregisterPaletteControlCallBack ();

    ~GraphEditorPalette () override;

  private:
    GraphEditorPalette ();

    void Show ();
    void Hide ();
    void LoadGraphEditorPage ();
    void RegisterJavaScriptObject ();

    void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;
    void PanelResized (const DG::PanelResizeEvent& ev) override;

    static GSErrCode PaletteControlCallBack (Int32 referenceId, API_PaletteMessageID messageId, GS::IntPtr param);

    static const GS::Guid paletteGuid;
    static GS::Ref<GraphEditorPalette> instance;

    DG::Browser browser;
};

#endif
