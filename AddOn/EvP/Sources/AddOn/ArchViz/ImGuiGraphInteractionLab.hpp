#ifndef EVP_ARCHVIZ_IMGUIGRAPHINTERACTIONLAB_HPP
#define EVP_ARCHVIZ_IMGUIGRAPHINTERACTIONLAB_HPP

#include <cstdint>

namespace geomsrv::archviz {

struct InputSnapshot;

// A deliberately isolated interaction proof of concept. It has no graph-runtime
// or Archicad dependency: it measures only Diligent presentation and ImGui input.
class ImGuiGraphInteractionLab final {
  public:
    void Draw (uint32_t width, uint32_t height, const InputSnapshot& input, uint32_t frameLatency, bool& open);

  private:
    float number_ = 12.0f;
    float multiplier_ = 2.0f;
    int repetitions_ = 3;
    bool clamp_ = false;
    uint64_t lastEventSequence_ = 0;
    uint64_t lastEventLatencyMs_ = 0;
};

} // namespace geomsrv::archviz

#endif
