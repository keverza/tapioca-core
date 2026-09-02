#include "ArchViz/SceneTextLayout.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

#include <limits>

namespace geomsrv::archviz {

struct SceneTextShaper::Impl {
    std::vector<uint8_t> fontBytes;
    FT_Library freetype = nullptr;
    FT_Face face = nullptr;
    hb_font_t* font = nullptr;

    ~Impl ()
    {
        if (font != nullptr)
            hb_font_destroy (font);
        if (face != nullptr)
            FT_Done_Face (face);
        if (freetype != nullptr)
            FT_Done_FreeType (freetype);
    }
};

SceneTextShaper::SceneTextShaper () : impl_ (new Impl ())
{
}
SceneTextShaper::~SceneTextShaper () = default;

bool SceneTextShaper::Init (const uint8_t* fontBytes, size_t fontByteCount, std::string& error)
{
    impl_.reset (new Impl ());
    if (fontBytes == nullptr || fontByteCount == 0 ||
        fontByteCount > static_cast<size_t> ((std::numeric_limits<FT_Long>::max) ())) {
        error = "the scene-text shaping font is empty or too large";
        return false;
    }
    impl_->fontBytes.assign (fontBytes, fontBytes + fontByteCount);
    if (FT_Init_FreeType (&impl_->freetype) != 0 ||
        FT_New_Memory_Face (impl_->freetype, impl_->fontBytes.data (), static_cast<FT_Long> (fontByteCount), 0,
                            &impl_->face) != 0) {
        error = "FreeType could not load the scene-text shaping font";
        return false;
    }
    impl_->font = hb_ft_font_create_referenced (impl_->face);
    if (impl_->font == nullptr || impl_->font == hb_font_get_empty ()) {
        error = "HarfBuzz could not create the scene-text font";
        return false;
    }
    const int unitsPerEm = static_cast<int> (impl_->face->units_per_EM);
    if (unitsPerEm <= 0) {
        error = "the scene-text font has no valid units-per-em metric";
        return false;
    }
    hb_font_set_scale (impl_->font, unitsPerEm, unitsPerEm);
    return true;
}

bool SceneTextShaper::Shape (const std::string& utf8, SceneTextDirection requestedDirection, SceneTextGlyphRun& run,
                             std::string& error) const
{
    run = {};
    if (impl_->font == nullptr) {
        error = "the scene-text shaper is not initialized";
        return false;
    }
    if (utf8.size () > static_cast<size_t> ((std::numeric_limits<int>::max) ())) {
        error = "the scene-text input is too large to shape";
        return false;
    }

    hb_buffer_t* buffer = hb_buffer_create ();
    if (buffer == nullptr || buffer == hb_buffer_get_empty ()) {
        error = "HarfBuzz could not allocate a shaping buffer";
        return false;
    }
    hb_buffer_set_replacement_codepoint (buffer, 0xFFFDu);
    hb_buffer_set_cluster_level (buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_buffer_add_utf8 (buffer, utf8.data (), static_cast<int> (utf8.size ()), 0, static_cast<int> (utf8.size ()));
    if (requestedDirection == SceneTextDirection::LeftToRight)
        hb_buffer_set_direction (buffer, HB_DIRECTION_LTR);
    else if (requestedDirection == SceneTextDirection::RightToLeft)
        hb_buffer_set_direction (buffer, HB_DIRECTION_RTL);
    hb_buffer_guess_segment_properties (buffer);
    hb_shape (impl_->font, buffer, nullptr, 0);

    unsigned int glyphCount = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos (buffer, &glyphCount);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions (buffer, &glyphCount);
    const float emScale = impl_->face->units_per_EM > 0 ? 1.0f / float (impl_->face->units_per_EM) : 0.0f;
    run.glyphs.reserve (glyphCount);
    for (unsigned int index = 0; index < glyphCount; ++index) {
        SceneTextPositionedGlyph glyph;
        glyph.glyphIndex = infos[index].codepoint;
        glyph.cluster = infos[index].cluster;
        glyph.xAdvance = float (positions[index].x_advance) * emScale;
        glyph.yAdvance = float (positions[index].y_advance) * emScale;
        glyph.xOffset = float (positions[index].x_offset) * emScale;
        glyph.yOffset = float (positions[index].y_offset) * emScale;
        run.advance += glyph.xAdvance;
        run.glyphs.push_back (glyph);
    }
    const hb_direction_t shapedDirection = hb_buffer_get_direction (buffer);
    run.direction =
        HB_DIRECTION_IS_BACKWARD (shapedDirection) ? SceneTextDirection::RightToLeft : SceneTextDirection::LeftToRight;
    hb_buffer_destroy (buffer);
    return true;
}

} // namespace geomsrv::archviz
