#ifndef EVP_ARCHVIZ_SCENETEXTLAYOUT_HPP
#define EVP_ARCHVIZ_SCENETEXTLAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz {

enum class SceneTextDirection : uint8_t { Auto, LeftToRight, RightToLeft };

// Font-independent coordinates in em units with y increasing above the baseline.
struct SceneTextMetricBounds {
    float left = 0.0f;
    float bottom = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
};

struct SceneTextPositionedGlyph {
    uint32_t glyphIndex = 0;
    uint32_t cluster = 0;
    float xAdvance = 0.0f;
    float yAdvance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
    SceneTextMetricBounds inkBounds;
    bool hasInkBounds = false;
};

struct SceneTextGlyphRun {
    std::vector<SceneTextPositionedGlyph> glyphs;
    SceneTextDirection direction = SceneTextDirection::LeftToRight;
    float advance = 0.0f;
    float baseline = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineHeight = 0.0f;
    SceneTextMetricBounds logicalBounds;
    SceneTextMetricBounds inkBounds;
    bool hasInkBounds = false;
};

// One shaper owns one FreeType face and is intentionally not concurrent. Worker
// pools create one instance per worker rather than locking a shared FT_Face.
class SceneTextShaper final {
  public:
    SceneTextShaper ();
    ~SceneTextShaper ();
    SceneTextShaper (const SceneTextShaper&) = delete;
    SceneTextShaper& operator= (const SceneTextShaper&) = delete;

    bool Init (const uint8_t* fontBytes, size_t fontByteCount, std::string& error);
    bool Shape (const std::string& utf8, SceneTextDirection requestedDirection, SceneTextGlyphRun& run,
                std::string& error) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geomsrv::archviz

#endif
