#ifndef EVP_ARCHVIZ_SCENETEXTATLAS_HPP
#define EVP_ARCHVIZ_SCENETEXTATLAS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace geomsrv::archviz {

struct SceneTextGlyph {
    uint32_t codepoint = 0;
    uint32_t glyphIndex = 0;
    float advance = 0.0f;
    float planeLeft = 0.0f;
    float planeBottom = 0.0f;
    float planeRight = 0.0f;
    float planeTop = 0.0f;
    float atlasLeft = 0.0f;
    float atlasBottom = 0.0f;
    float atlasRight = 0.0f;
    float atlasTop = 0.0f;
};

class SceneTextAtlas final {
  public:
    static constexpr int kMaximumDimension = 1024;
    static constexpr float kEmPixels = 40.0f;
    static constexpr float kDistanceRangePixels = 4.0f;

    bool Build (const uint8_t* fontBytes, size_t fontByteCount, std::string& error);
    const SceneTextGlyph* Find (uint32_t codepoint) const;
    const SceneTextGlyph* FindGlyph (uint32_t glyphIndex) const;

    int Width () const
    {
        return width_;
    }
    int Height () const
    {
        return height_;
    }
    const std::vector<uint8_t>& Pixels () const
    {
        return pixels_;
    }
    size_t GlyphCount () const
    {
        return glyphs_.size ();
    }

  private:
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> pixels_;
    std::unordered_map<uint32_t, SceneTextGlyph> glyphs_;
    std::unordered_map<uint32_t, uint32_t> codepointToGlyph_;
    uint32_t replacementGlyphIndex_ = 0;
};

// Strict UTF-8 decoding used by both retained labels and atlas tests. Invalid
// byte sequences consume one byte and become U+FFFD, so malformed input cannot
// make the renderer loop forever or index outside the atlas.
std::vector<uint32_t> DecodeSceneTextUtf8 (const std::string& text);

} // namespace geomsrv::archviz

#endif
