#ifndef EVP_ARCHVIZ_SCENETEXTLAYOUT_HPP
#define EVP_ARCHVIZ_SCENETEXTLAYOUT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz {

enum class SceneTextDirection : uint8_t { Auto, LeftToRight, RightToLeft };

struct SceneTextPositionedGlyph {
    uint32_t glyphIndex = 0;
    uint32_t cluster = 0;
    float xAdvance = 0.0f;
    float yAdvance = 0.0f;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
};

struct SceneTextGlyphRun {
    std::vector<SceneTextPositionedGlyph> glyphs;
    SceneTextDirection direction = SceneTextDirection::LeftToRight;
    float advance = 0.0f;
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
