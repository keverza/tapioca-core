#include "ArchViz/SceneTextAtlas.hpp"

#include "ArchViz/SceneTextLayout.hpp"

#include <msdf-atlas-gen/msdf-atlas-gen.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace geomsrv::archviz {
namespace {

constexpr uint32_t kReplacementCodepoint = 0xFFFDu;

std::vector<uint32_t> SeedCodepoints ()
{
    std::vector<uint32_t> codepoints;
    for (uint32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
        codepoints.push_back (codepoint);
    constexpr uint32_t extras[] = {
        0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00D7, 0x0104, 0x0105, 0x010C, 0x010D,
        0x0116, 0x0117, 0x0118, 0x0119, 0x012E, 0x012F, 0x0160, 0x0161, 0x0172,
        0x0173, 0x017D, 0x017E, 0x2190, 0x2191, 0x2192, 0x2193, 0x2300, kReplacementCodepoint,
    };
    codepoints.insert (codepoints.end (), std::begin (extras), std::end (extras));
    return codepoints;
}

std::string EncodeUtf8 (const std::vector<uint32_t>& codepoints)
{
    std::string text;
    for (uint32_t codepoint : codepoints) {
        if (codepoint <= 0x7Fu)
            text.push_back (static_cast<char> (codepoint));
        else if (codepoint <= 0x7FFu) {
            text.push_back (static_cast<char> (0xC0u | (codepoint >> 6)));
            text.push_back (static_cast<char> (0x80u | (codepoint & 0x3Fu)));
        }
        else {
            text.push_back (static_cast<char> (0xE0u | (codepoint >> 12)));
            text.push_back (static_cast<char> (0x80u | ((codepoint >> 6) & 0x3Fu)));
            text.push_back (static_cast<char> (0x80u | (codepoint & 0x3Fu)));
        }
    }
    return text;
}

} // namespace

std::vector<uint32_t> DecodeSceneTextUtf8 (const std::string& text)
{
    std::vector<uint32_t> codepoints;
    codepoints.reserve (text.size ());
    for (size_t index = 0; index < text.size ();) {
        const uint8_t first = static_cast<uint8_t> (text[index]);
        uint32_t value = 0;
        size_t length = 0;
        uint32_t minimum = 0;
        if (first < 0x80) {
            value = first;
            length = 1;
        }
        else if ((first & 0xE0) == 0xC0) {
            value = first & 0x1F;
            length = 2;
            minimum = 0x80;
        }
        else if ((first & 0xF0) == 0xE0) {
            value = first & 0x0F;
            length = 3;
            minimum = 0x800;
        }
        else if ((first & 0xF8) == 0xF0) {
            value = first & 0x07;
            length = 4;
            minimum = 0x10000;
        }
        if (length == 0 || index + length > text.size ()) {
            codepoints.push_back (kReplacementCodepoint);
            ++index;
            continue;
        }
        bool valid = true;
        for (size_t offset = 1; offset < length; ++offset) {
            const uint8_t continuation = static_cast<uint8_t> (text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            value = (value << 6) | (continuation & 0x3F);
        }
        if (!valid || value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            codepoints.push_back (kReplacementCodepoint);
            ++index;
            continue;
        }
        codepoints.push_back (value);
        index += length;
    }
    return codepoints;
}

bool SceneTextAtlas::Build (const uint8_t* fontBytes, size_t fontByteCount, std::string& error)
{
    width_ = height_ = 0;
    pixels_.clear ();
    glyphs_.clear ();
    codepointToGlyph_.clear ();
    replacementGlyphIndex_ = 0;
    if (fontBytes == nullptr || fontByteCount == 0 ||
        fontByteCount > static_cast<size_t> ((std::numeric_limits<int>::max) ())) {
        error = "the bundled text font is empty or too large";
        return false;
    }

    msdfgen::FreetypeHandle* freetype = msdfgen::initializeFreetype ();
    if (freetype == nullptr) {
        error = "msdfgen could not initialize FreeType";
        return false;
    }
    msdfgen::FontHandle* font = msdfgen::loadFontData (freetype, fontBytes, static_cast<int> (fontByteCount));
    if (font == nullptr) {
        msdfgen::deinitializeFreetype (freetype);
        error = "FreeType could not load the bundled Noto Sans font";
        return false;
    }

    SceneTextShaper shaper;
    if (!shaper.Init (fontBytes, fontByteCount, error)) {
        msdfgen::destroyFont (font);
        msdfgen::deinitializeFreetype (freetype);
        return false;
    }
    std::vector<msdf_atlas::GlyphGeometry> sourceGlyphs;
    msdf_atlas::FontGeometry fontGeometry (&sourceGlyphs);
    msdf_atlas::Charset glyphset;
    const std::vector<uint32_t> seedCodepoints = SeedCodepoints ();
    for (uint32_t codepoint : seedCodepoints) {
        msdfgen::GlyphIndex glyphIndex;
        if (msdfgen::getGlyphIndex (glyphIndex, font, codepoint)) {
            glyphset.add (glyphIndex.getIndex ());
            codepointToGlyph_.emplace (codepoint, glyphIndex.getIndex ());
            if (codepoint == kReplacementCodepoint)
                replacementGlyphIndex_ = glyphIndex.getIndex ();
        }
    }
    SceneTextGlyphRun seedRun;
    if (!shaper.Shape (EncodeUtf8 (seedCodepoints) + " office affine fi fl ffi ffl a\xCC\x81", SceneTextDirection::Auto,
                       seedRun, error)) {
        msdfgen::destroyFont (font);
        msdfgen::deinitializeFreetype (freetype);
        return false;
    }
    for (const SceneTextPositionedGlyph& glyph : seedRun.glyphs)
        glyphset.add (glyph.glyphIndex);
    const int loaded = fontGeometry.loadGlyphset (font, 1.0, glyphset, true, false);
    msdfgen::destroyFont (font);
    msdfgen::deinitializeFreetype (freetype);
    if (loaded <= 0 || sourceGlyphs.empty ()) {
        error = "the bundled font yielded no scene-text glyphs";
        return false;
    }

    msdf_atlas::TightAtlasPacker packer;
    packer.setDimensionsConstraint (msdf_atlas::DimensionsConstraint::POWER_OF_TWO_SQUARE);
    packer.setScale (kEmPixels);
    packer.setPixelRange (kDistanceRangePixels);
    packer.setSpacing (2);
    packer.setOuterPixelPadding (msdf_atlas::Padding (1.0));
    if (packer.pack (sourceGlyphs.data (), static_cast<int> (sourceGlyphs.size ())) != 0) {
        error = "the seed glyph set did not fit its MTSDF atlas";
        return false;
    }
    packer.getDimensions (width_, height_);
    if (width_ <= 0 || height_ <= 0 || width_ > kMaximumDimension || height_ > kMaximumDimension) {
        error = "the seed MTSDF atlas exceeded its 1024 pixel bound";
        width_ = height_ = 0;
        return false;
    }

    unsigned long long seed = 1;
    for (msdf_atlas::GlyphGeometry& glyph : sourceGlyphs) {
        glyph.edgeColoring (msdfgen::edgeColoringInkTrap, 3.0, seed);
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    }
    using Storage = msdf_atlas::BitmapAtlasStorage<msdf_atlas::byte, 4>;
    using Generator = msdf_atlas::ImmediateAtlasGenerator<float, 4, msdf_atlas::mtsdfGenerator, Storage>;
    Generator generator (width_, height_);
    generator.setThreadCount (1);
    generator.generate (sourceGlyphs.data (), static_cast<int> (sourceGlyphs.size ()));

    const msdfgen::BitmapConstSection<msdf_atlas::byte, 4> bitmap = generator.atlasStorage ();
    pixels_.resize (static_cast<size_t> (width_) * static_cast<size_t> (height_) * 4);
    for (int y = 0; y < height_; ++y)
        std::memcpy (pixels_.data () + static_cast<size_t> (y) * width_ * 4, bitmap (0, y),
                     static_cast<size_t> (width_) * 4);

    for (const msdf_atlas::GlyphGeometry& source : sourceGlyphs) {
        SceneTextGlyph glyph;
        glyph.codepoint = source.getCodepoint ();
        glyph.glyphIndex = static_cast<uint32_t> (source.getIndex ());
        glyph.advance = static_cast<float> (source.getAdvance ());
        double left = 0.0, bottom = 0.0, right = 0.0, top = 0.0;
        source.getQuadPlaneBounds (left, bottom, right, top);
        glyph.planeLeft = static_cast<float> (left);
        glyph.planeBottom = static_cast<float> (bottom);
        glyph.planeRight = static_cast<float> (right);
        glyph.planeTop = static_cast<float> (top);
        source.getQuadAtlasBounds (left, bottom, right, top);
        glyph.atlasLeft = static_cast<float> (left);
        glyph.atlasBottom = static_cast<float> (bottom);
        glyph.atlasRight = static_cast<float> (right);
        glyph.atlasTop = static_cast<float> (top);
        glyphs_.emplace (glyph.glyphIndex, glyph);
    }
    return true;
}

const SceneTextGlyph* SceneTextAtlas::Find (uint32_t codepoint) const
{
    const auto mapped = codepointToGlyph_.find (codepoint);
    return FindGlyph (mapped != codepointToGlyph_.end () ? mapped->second : replacementGlyphIndex_);
}

const SceneTextGlyph* SceneTextAtlas::FindGlyph (uint32_t glyphIndex) const
{
    auto found = glyphs_.find (glyphIndex);
    if (found == glyphs_.end ())
        found = glyphs_.find (replacementGlyphIndex_);
    return found != glyphs_.end () ? &found->second : nullptr;
}

} // namespace geomsrv::archviz
