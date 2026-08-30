#ifndef EVP_ARCHVIZ_IMGUIGRAPHINTERACTIONLAB_HPP
#define EVP_ARCHVIZ_IMGUIGRAPHINTERACTIONLAB_HPP

#include <cstdint>
#include <memory>

namespace geomsrv::archviz {

struct InputSnapshot;

// A deliberately isolated interaction proof of concept. It has no graph-runtime
// or Archicad dependency: it measures only Diligent presentation and ImGui input.
class ImGuiGraphInteractionLab final {
  public:
    ImGuiGraphInteractionLab ();
    ~ImGuiGraphInteractionLab ();
    ImGuiGraphInteractionLab (const ImGuiGraphInteractionLab&) = delete;
    ImGuiGraphInteractionLab& operator= (const ImGuiGraphInteractionLab&) = delete;

    void Draw (uint32_t width, uint32_t height, const InputSnapshot& input, uint32_t frameLatency, bool& open,
               bool& fastPath);

  private:
    float number_ = 12.0f;
    float multiplier_ = 2.0f;
    int repetitions_ = 3;
    bool clamp_ = false;
    bool wireDragging_ = false;
    float canvasPanX_ = 0.0f;
    float canvasPanY_ = 0.0f;
    float canvasZoom_ = 1.0f;
    bool telemetryEnabled_ = false;
    uint64_t lastEventSequence_ = 0;
    uint64_t lastEventLatencyUs_ = 0;
    uint64_t lastPointerQpc_ = 0;
    uint64_t lastPointerMessageQpc_ = 0;
    uint64_t lastFrameQpc_ = 0;
    uint64_t lastMoveQpc_ = 0;
    uint64_t pointerSampleGapUs_ = 0;
    uint64_t pointerSampleAgeUs_ = 0;
    uint64_t pointerMessageGapUs_ = 0;
    uint64_t pointerMessageAgeUs_ = 0;
    uint64_t pointerMoveGapUs_ = 0;
    uint64_t frameGapUs_ = 0;
    int32_t lastPointerX_ = 0;
    int32_t lastPointerY_ = 0;
    struct Telemetry;
    std::unique_ptr<Telemetry> telemetry_;
};

} // namespace geomsrv::archviz

#endif
