#ifndef EVP_PALETTE_WEBUIPALETTE_HPP
#define EVP_PALETTE_WEBUIPALETTE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGBrowser.hpp"
#include "DGModule.hpp"

class WebUIPalette final : public DG::Palette, public DG::PanelObserver {
  public:
    static bool HasInstance ();
    static void CreateInstance ();
    static WebUIPalette& GetInstance ();
    static void DestroyInstance ();

    static void Open ();

    static GSErrCode RegisterPaletteControlCallBack ();
    static GSErrCode UnregisterPaletteControlCallBack ();

    ~WebUIPalette () override;

  private:
    WebUIPalette ();

    void Show ();
    void Hide ();
    void LoadWebUIPage ();
    void RegisterJavaScriptObject ();

    void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;
    void PanelResized (const DG::PanelResizeEvent& ev) override;

    static GSErrCode PaletteControlCallBack (Int32 referenceId, API_PaletteMessageID messageId, GS::IntPtr param);

    static const GS::Guid paletteGuid;
    static GS::Ref<WebUIPalette> instance;

    DG::Browser browser;
    GS::UniString fallbackHtml;
    bool serverPageRequested = false;
    bool fallbackPageLoaded = false;
};

#endif
