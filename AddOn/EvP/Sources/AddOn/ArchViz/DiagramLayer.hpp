#ifndef EVP_ARCHVIZ_DIAGRAMLAYER_HPP
#define EVP_ARCHVIZ_DIAGRAMLAYER_HPP
// ArchViz/DiagramLayer -- the GPU half of the exploded diagram (#201 §5.8).
//
// It owns ONLY resources: one vertex/index buffer pair holding every node's node-local
// shell, one constant buffer, and one pipeline. The geometry comes from DiagramGeometry,
// which is pure and tested offline; the poses come from diagram::Evaluate. Same split as
// GhPreviewLayer and StorySliceLayer.
//
// ⚠️ GEOMETRY IS UPLOADED ONCE PER PUBLICATION, NEVER PER FRAME. Animating t changes only
// the per-draw translation in the constant buffer: one map per visible node, zero vertex
// bytes. `UploadCount` exists so a test or the HUD can say so.
//
// ⚠️ IT IS NOT DiligentScene's MESH PIPELINE. The diagram is a presentation of the
// building, not the building: flat legibility shading, no shadows, no AO, no reflections,
// and nothing here enters the pick or G-buffer passes. It depth-tests against the model
// so terrain and neighbours still occlude it, and it is pulled a few depth units toward
// the camera so that at t = 0, with the source left visible, it wins the coincident faces
// instead of z-fighting them.
//
// ⚠️ RENDER THREAD ONLY. No ACAPI, no camera of its own.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Diligent {
struct IDeviceContext;
struct IRenderDevice;
} // namespace Diligent

// Forward-declared: DiligentSceneImpl.hpp holds a DiagramLayer, and every scene TU must not
// pull the massing headers (and ImGui behind them) in with it.
namespace geomsrv::archviz::diagram {
struct DiagramGeometry;
struct DiagramDraw;
struct DiagramLineDraw;
} // namespace geomsrv::archviz::diagram

namespace geomsrv::archviz {
class DiagramLayer final {
  public:
    DiagramLayer ();
    ~DiagramLayer ();
    DiagramLayer (const DiagramLayer&) = delete;
    DiagramLayer& operator= (const DiagramLayer&) = delete;

    bool Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
               std::string& error);
    void Shutdown ();
    bool IsReady () const;

    // Replaces the held geometry. An empty geometry is ordinary: nothing to draw.
    bool Upload (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context,
                 const diagram::DiagramGeometry& geometry, std::string& error);

    struct DrawParams {
        float ambient = 0.5f;
        uint32_t selectedRgba = 0x2BB3E6FF;  // diagram-owned highlight (§5.9): one highlight source
        float monochrome = 0.0f;             // 1: one white study-model tone instead of function colours
        uint32_t outlineRgba = 0x2C3036FF;   // edges of the shells
        uint32_t connectorRgba = 0x5A6068FF; // dashed corner lines between storeys
        float dashMetres = 0.5f;             // one dash plus one gap
    };
    // Shells first, then lines over them: an outline is pulled further toward the camera
    // than its faces, so it wins the coincident edge, and it writes no depth.
    void Draw (Diligent::IDeviceContext* context, const float viewProj[16],
               const std::vector<diagram::DiagramDraw>& draws, const std::vector<diagram::DiagramLineDraw>& lines,
               const DrawParams& params);

    uint64_t UploadCount () const; // geometry uploads since Init
    size_t UploadedBytes () const;
    size_t DrawCount () const;     // shell draws issued by the last Draw
    size_t LineDrawCount () const; // outline and connector draws issued by the last Draw

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace geomsrv::archviz
#endif
