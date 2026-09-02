#ifndef EVP_ARCHVIZ_SCENETEXTLAYER_HPP
#define EVP_ARCHVIZ_SCENETEXTLAYER_HPP

#include "ArchViz/TraceAnnotationLayer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

namespace geomsrv::archviz {

enum class SceneTextAlignment : uint8_t { Left, Center, Right };

struct SceneTextLabel {
    double anchor[3] = {};
    std::string text;
    float sizePixels = 18.0f;
    uint32_t rgba = 0xFFFFFFFFu;
    SceneTextAlignment alignment = SceneTextAlignment::Center;
};

struct SceneTextLayerStats {
    bool ready = false;
    uint64_t labels = 0;
    uint64_t glyphs = 0;
    uint64_t atlasBytes = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
};

class SceneTextLayer final {
  public:
    SceneTextLayer ();
    ~SceneTextLayer ();
    SceneTextLayer (const SceneTextLayer&) = delete;
    SceneTextLayer& operator= (const SceneTextLayer&) = delete;

    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);
    void Shutdown ();
    void Draw (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
               const std::vector<SceneTextLabel>& labels, const float viewProj[16], uint32_t surfaceWidth,
               uint32_t surfaceHeight, float dpiScale);
    void DrawProjected (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                        const std::vector<ScreenLabel>& labels, uint32_t surfaceWidth, uint32_t surfaceHeight,
                        float dpiScale);
    bool IsReady () const;
    SceneTextLayerStats Stats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geomsrv::archviz

#endif
