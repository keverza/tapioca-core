#ifndef TAPIOCA_ARCHVIZ_VIEWEREDITORDRAW_HPP
#define TAPIOCA_ARCHVIZ_VIEWEREDITORDRAW_HPP

// OVERLAY-INVARIANTS.md §12b: these resources belong to the standalone viewer's
// device. No attach to, snapshot of, or retained resource from Archicad is used.
#include "ArchViz/Dxgi/GuestGpu.hpp"
#include "ArchViz/ViewerEditor.hpp"

namespace geomsrv::archviz {
class DiligentScene;
class ViewerEditorDraw {
  public:
    explicit ViewerEditorDraw (bool standalone) : standalone_ (standalone)
    {
    }
    size_t ConsumeScene (DiligentScene& scene, Diligent::IRenderDevice* device, Diligent::IDeviceContext* context);
    void Release ();
    void Update (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::ITextureView* target,
                 Diligent::ITextureView* depth, const float* view, const float* projection, uint32_t width,
                 uint32_t height, bool showSurfaceHighlight);
    bool Draw (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, Diligent::TEXTURE_FORMAT colour,
               Diligent::TEXTURE_FORMAT depth, const vieweredit::Frame& frame, bool editorActive,
               const std::shared_ptr<const vieweredit::SurfaceHighlight>& highlight, std::string& error);

  private:
    void Clear ();
    bool standalone_;
    std::shared_ptr<const Snapshot> capture_;
    dxgi::guestgpu::Pipelines pipelines_;
    dxgi::guestgpu::Content scene_, hud_, surface_;
    std::vector<dxgi::guestgpu::Page> pages_, hudPages_, surfacePages_;
    std::shared_ptr<const vieweredit::SurfaceHighlight> uploadedSurface_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer> view_, projection_;
    float scale_ = 1;
    bool ready_ = false;
    uint64_t generation_ = 0;
};
} // namespace geomsrv::archviz
#endif
