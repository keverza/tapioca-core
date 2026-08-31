#ifndef EVP_PALETTE_WEBVIEW2GRAPHHOST_HPP
#define EVP_PALETTE_WEBVIEW2GRAPHHOST_HPP

#include "UniString.hpp"

#include <memory>

struct HWND__;
struct WebView2GraphHostState;

class WebView2GraphHost final {
  public:
    WebView2GraphHost ();
    ~WebView2GraphHost ();

    WebView2GraphHost (const WebView2GraphHost&) = delete;
    WebView2GraphHost& operator= (const WebView2GraphHost&) = delete;

    bool Start (HWND__* parent, const GS::UniString& html);
    void Resize ();
    void SetVisible (bool visible);
    void Focus ();
    void RefreshNativeTelemetry ();

  private:
    std::shared_ptr<WebView2GraphHostState> state_;
};

#endif
