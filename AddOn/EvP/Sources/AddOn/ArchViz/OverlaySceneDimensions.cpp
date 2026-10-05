#include "ArchViz/OverlaySceneBuilder.hpp"

namespace geomsrv::archviz::overlayscene::build {
void Builder::AddDimensionText (const layers::Layer& layer, const layers::Dimension& dimension, double measurement,
                                const Vec3& middle, const Vec3& span, const Vec3& normal)
{
    const std::string text = !dimension.text.empty ()
                                 ? dimension.text
                                 : FormatLength (measurement, dimension.decimals, dimension.unit, dimension.showUnit);
    if (dimension.textSizeMetres == 0) {
        overlaytext::Label label;
        if (!LayOut (text, dimension.textSizePixels, layers::Align::Center, layers::Baseline::Bottom, label,
                     dimension.font))
            return;
        const float minSpan = (label.right - label.left) + 20.0f;
        for (const auto& quad : label.quads) {
            DraftGlyph glyph;
            Assign (glyph.anchor, middle);
            Assign (glyph.dir, span);
            glyph.rgba = (dimension.textRgba & 0xFFu) != 0 ? dimension.textRgba : dimension.rgba;
            glyph.halo = dimension.haloRgba;
            glyph.haloPixels = HaloOf (dimension.haloPixels, 1.0f);
            glyph.flags = kAlongDirection | kKeepUpright | kHideShortSpan;
            glyph.minSpan = minSpan;
            glyph.behind = BehindOf (dimension.behind, layer);
            PushQuad (glyph, quad, 0, -3.0f);
        }
        return;
    }
    layers::Text model;
    model.text = text;
    model.planar = true;
    model.sizeMetres = dimension.textSizeMetres;
    model.sizePixels = dimension.textSizePixels;
    model.minProjectedPixels = dimension.textMinProjectedPixels;
    model.rgba = (dimension.textRgba & 0xFFu) != 0 ? dimension.textRgba : dimension.rgba;
    model.haloRgba = dimension.haloRgba;
    model.haloPixels = dimension.haloPixels;
    model.font = dimension.font;
    model.behind = dimension.behind;
    const auto baseline = Unit (span), plane = Unit (normal);
    const auto up = Unit (Cross (plane, baseline));
    const double gap = dimension.textSizeMetres * 0.75;
    const Vec3 at { middle.x + up.x * gap, middle.y + up.y * gap, middle.z + up.z * gap };
    Assign (model.at, at);
    Assign (model.direction, baseline);
    Assign (model.normal, plane);
    AddText (layer, model);
}
} // namespace geomsrv::archviz::overlayscene::build
