// ArchViz/OverlayText -- see the header.

#include "ArchViz/OverlayText.hpp"

#include "ArchViz/SceneTextAtlas.hpp"
#include "ArchViz/SceneTextLayout.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace geomsrv {
namespace archviz {
namespace overlaytext {

namespace {

// A label longer than this is cut: the viewer's own limit (SceneTextLayer).
constexpr size_t kMaxGlyphsPerLabel = 512;
// Pages beyond the seed. 64 glyphs each; past this every new glyph is the
// replacement glyph, counted -- a bound on what one caller's text can allocate.
constexpr size_t kMaxExtraPages = 24;

std::atomic<uint64_t> g_nextPageId { 1 };

std::shared_ptr<const Page> MakePage (int width, int height, const std::vector<uint8_t>& pixels)
{
    auto page = std::make_shared<Page> ();
    page->id = g_nextPageId.fetch_add (1, std::memory_order_relaxed);
    page->width = width;
    page->height = height;
    page->pixels = pixels;
    return page;
}

} // namespace

struct Engine::Impl {
    std::vector<uint8_t> font;
    SceneTextShaper shaper;
    SceneTextAtlas seed;
    std::vector<std::shared_ptr<const SceneTextAtlasPage>> extra; // page i + 1
    std::vector<std::shared_ptr<const Page>> pages;
    std::unordered_set<uint32_t> unavailable; // glyphs a page generation refused
    bool ready = false;
    Stats stats;

    // The glyph's metrics and the page holding it, or nullptr when no page does.
    const SceneTextGlyph* Find (uint32_t glyphIndex, uint32_t& page) const
    {
        if (const SceneTextGlyph* glyph = seed.FindGlyphExact (glyphIndex)) {
            page = 0;
            return glyph;
        }
        for (size_t i = 0; i < extra.size (); ++i) {
            if (const SceneTextGlyph* glyph = extra[i]->FindGlyphExact (glyphIndex)) {
                page = uint32_t (i + 1);
                return glyph;
            }
        }
        return nullptr;
    }

    void Generate (std::vector<uint32_t> missing)
    {
        std::sort (missing.begin (), missing.end ());
        missing.erase (std::unique (missing.begin (), missing.end ()), missing.end ());
        for (size_t begin = 0; begin < missing.size (); begin += SceneTextAtlasPage::kMaximumGlyphs) {
            const size_t end = (std::min) (begin + SceneTextAtlasPage::kMaximumGlyphs, missing.size ());
            const std::vector<uint32_t> batch (missing.begin () + begin, missing.begin () + end);
            if (extra.size () >= kMaxExtraPages) {
                unavailable.insert (batch.begin (), batch.end ());
                continue;
            }
            std::string error;
            std::shared_ptr<const SceneTextAtlasPage> page =
                GenerateSceneTextAtlasPage (font.data (), font.size (), batch, error);
            if (page == nullptr || page->Width () <= 0 || page->Height () <= 0) {
                unavailable.insert (batch.begin (), batch.end ());
                continue;
            }
            for (const uint32_t glyph : batch)
                if (page->FindGlyphExact (glyph) == nullptr)
                    unavailable.insert (glyph);
            stats.generatedGlyphs += batch.size ();
            extra.push_back (page);
            pages.push_back (MakePage (page->Width (), page->Height (), page->Pixels ()));
        }
    }
};

float Engine::DistanceRangePixels ()
{
    return SceneTextAtlas::kDistanceRangePixels;
}

Engine::Engine () : impl_ (new Impl ())
{
}

Engine::~Engine () = default;

bool Engine::Init (std::vector<uint8_t> fontBytes, std::string& error)
{
    if (impl_->ready)
        return true;
    const auto started = std::chrono::steady_clock::now ();
    impl_->font = std::move (fontBytes);
    if (impl_->font.empty ()) {
        error = "the overlay text font is empty";
        return false;
    }
    if (!impl_->shaper.Init (impl_->font.data (), impl_->font.size (), error))
        return false;
    SceneTextGlyphRun seedRun;
    if (!impl_->shaper.Shape (SceneTextSeedText (), SceneTextDirection::Auto, seedRun, error) ||
        !impl_->seed.Build (impl_->font.data (), impl_->font.size (), seedRun, error))
        return false;
    impl_->pages.push_back (MakePage (impl_->seed.Width (), impl_->seed.Height (), impl_->seed.Pixels ()));
    impl_->ready = true;
    impl_->stats.seedMilliseconds = uint32_t (
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - started).count ());
    return true;
}

bool Engine::Ready () const
{
    return impl_->ready;
}

const std::vector<std::shared_ptr<const Page>>& Engine::Pages () const
{
    return impl_->pages;
}

Stats Engine::GetStats () const
{
    Stats stats = impl_->stats;
    stats.pages = uint32_t (impl_->pages.size ());
    return stats;
}

bool Engine::Layout (const std::string& utf8, float sizePixels, overlaylayers::Align align,
                     overlaylayers::Baseline baseline, Label& out, std::string& error)
{
    out = Label {};
    if (!impl_->ready) {
        error = "the overlay text engine has no font";
        return false;
    }
    if (!(std::isfinite (sizePixels) && sizePixels > 0.0f)) {
        error = "a text size is a positive number of pixels";
        return false;
    }
    SceneTextGlyphRun run;
    if (!impl_->shaper.Shape (utf8, SceneTextDirection::Auto, run, error))
        return false;
    ++impl_->stats.layouts;
    const size_t count = (std::min) (run.glyphs.size (), kMaxGlyphsPerLabel);

    // Everything missing is generated first, in as few pages as it takes, so one
    // label never spreads over pages made one glyph at a time.
    std::vector<uint32_t> missing;
    for (size_t i = 0; i < count; ++i) {
        uint32_t page = 0;
        const uint32_t glyph = run.glyphs[i].glyphIndex;
        if (impl_->Find (glyph, page) == nullptr && impl_->unavailable.count (glyph) == 0)
            missing.push_back (glyph);
    }
    if (!missing.empty ())
        impl_->Generate (std::move (missing));

    struct Placed {
        const SceneTextGlyph* glyph;
        uint32_t page;
        float pen;
        float xOffset;
        float yOffset;
    };
    std::vector<Placed> placed;
    placed.reserve (count);
    float pen = 0.0f;
    float inkTop = 0.0f, inkBottom = 0.0f;
    bool anyInk = false;
    for (size_t i = 0; i < count; ++i) {
        const SceneTextPositionedGlyph& positioned = run.glyphs[i];
        uint32_t page = 0;
        const SceneTextGlyph* glyph = impl_->Find (positioned.glyphIndex, page);
        if (glyph == nullptr) {
            // The seed's replacement glyph: something is drawn, and it is counted.
            glyph = impl_->seed.FindGlyph (positioned.glyphIndex);
            page = 0;
            ++out.replaced;
            ++impl_->stats.replacedGlyphs;
        }
        if (glyph != nullptr) {
            placed.push_back ({ glyph, page, pen, positioned.xOffset, positioned.yOffset });
            if (glyph->planeRight > glyph->planeLeft && glyph->planeTop > glyph->planeBottom) {
                const float top = positioned.yOffset + glyph->planeTop;
                const float bottom = positioned.yOffset + glyph->planeBottom;
                inkTop = anyInk ? (std::max) (inkTop, top) : top;
                inkBottom = anyInk ? (std::min) (inkBottom, bottom) : bottom;
                anyInk = true;
            }
        }
        pen += positioned.xAdvance;
    }

    // The same arithmetic as SceneTextLayer::DrawPrepared, in em, then scaled: the
    // anchor is x = 0 on the run's start, centre or end, and y = 0 on the baseline
    // moved to the ink's top, middle or bottom. Em units are y up; pixels are y down.
    const float advance = pen;
    float startX = 0.0f;
    if (align == overlaylayers::Align::Center)
        startX = -advance * 0.5f;
    else if (align == overlaylayers::Align::Right)
        startX = -advance;
    float baselineY = 0.0f;
    if (anyInk) {
        if (baseline == overlaylayers::Baseline::Top)
            baselineY = inkTop;
        else if (baseline == overlaylayers::Baseline::Bottom)
            baselineY = inkBottom;
        else if (baseline == overlaylayers::Baseline::Middle)
            baselineY = (inkTop + inkBottom) * 0.5f;
    }

    bool haveBounds = false;
    for (const Placed& p : placed) {
        const SceneTextGlyph& g = *p.glyph;
        if (!(g.planeRight > g.planeLeft && g.planeTop > g.planeBottom))
            continue; // a space: advance only
        const std::shared_ptr<const Page>& page = impl_->pages[p.page];
        const float width = float (page->width);
        const float height = float (page->height);
        Quad quad;
        quad.left = (startX + p.pen + p.xOffset + g.planeLeft) * sizePixels;
        quad.right = (startX + p.pen + p.xOffset + g.planeRight) * sizePixels;
        quad.top = (baselineY - (p.yOffset + g.planeTop)) * sizePixels;
        quad.bottom = (baselineY - (p.yOffset + g.planeBottom)) * sizePixels;
        // As SceneTextLayer's AddQuad: the quad's top samples atlasTop, its bottom
        // atlasBottom -- the atlas's rows are stored the way its generator wrote them.
        quad.u0 = g.atlasLeft / width;
        quad.u1 = g.atlasRight / width;
        quad.v0 = g.atlasTop / height;
        quad.v1 = g.atlasBottom / height;
        quad.page = p.page;
        out.quads.push_back (quad);
        if (!haveBounds) {
            out.left = quad.left;
            out.right = quad.right;
            out.top = quad.top;
            out.bottom = quad.bottom;
            haveBounds = true;
        }
        else {
            out.left = (std::min) (out.left, quad.left);
            out.right = (std::max) (out.right, quad.right);
            out.top = (std::min) (out.top, quad.top);
            out.bottom = (std::max) (out.bottom, quad.bottom);
        }
    }
    return true;
}

} // namespace overlaytext
} // namespace archviz
} // namespace geomsrv
