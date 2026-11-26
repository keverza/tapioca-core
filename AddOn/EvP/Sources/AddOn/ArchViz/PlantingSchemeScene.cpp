#include "ArchViz/PlantingSchemeScene.hpp"

#include "ArchViz/SunPathScene.hpp"

#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::planting {

namespace {

constexpr int kSides = 24;
constexpr double kLift = 0.01; // above the host surface, so the base never fights it
constexpr double kTwoPi = 6.28318530717958647692;

// A species colour as display (sRGB) components, linearised for this channel.
uint32_t Display (uint32_t rgba, int alpha)
{
    return SunPathDisplayRgba (int ((rgba >> 24) & 0xff), int ((rgba >> 16) & 0xff), int ((rgba >> 8) & 0xff), alpha);
}

void Vertex (GhPreviewBucket& b, double x, double y, double z, float nx, float ny, float nz, uint32_t rgba)
{
    b.meshVertices.push_back ({ float (x), float (y), float (z), nx, ny, nz, rgba });
}

void AppendCylinder (const PlacedPlant& p, double height, uint32_t rgba, GhPreviewBucket& b)
{
    const double z0 = p.z + kLift, z1 = p.z + kLift + height;
    const uint32_t base = uint32_t (b.meshVertices.size ());
    for (int i = 0; i <= kSides; ++i) {
        const double a = kTwoPi * i / kSides;
        const float c = float (std::cos (a)), s = float (std::sin (a));
        Vertex (b, p.x + p.radius * c, p.y + p.radius * s, z0, c, s, 0, rgba);
        Vertex (b, p.x + p.radius * c, p.y + p.radius * s, z1, c, s, 0, rgba);
    }
    for (uint32_t i = 0; i < uint32_t (kSides); ++i) {
        const uint32_t k = base + 2 * i;
        b.meshIndices.insert (b.meshIndices.end (), { k, k + 2, k + 1, k + 1, k + 2, k + 3 });
    }
    // The top cap, a fan around the centre.
    const uint32_t centre = uint32_t (b.meshVertices.size ());
    Vertex (b, p.x, p.y, z1, 0, 0, 1, rgba);
    for (int i = 0; i <= kSides; ++i) {
        const double a = kTwoPi * i / kSides;
        Vertex (b, p.x + p.radius * std::cos (a), p.y + p.radius * std::sin (a), z1, 0, 0, 1, rgba);
    }
    for (uint32_t i = 0; i < uint32_t (kSides); ++i)
        b.meshIndices.insert (b.meshIndices.end (), { centre, centre + 1 + i, centre + 2 + i });
}

void AppendRim (const PlacedPlant& p, double z, uint32_t rgba, GhPreviewBucket& b)
{
    for (int i = 0; i < kSides; ++i) {
        const double a0 = kTwoPi * i / kSides, a1 = kTwoPi * (i + 1) / kSides;
        const float from[3] = { float (p.x + p.radius * std::cos (a0)), float (p.y + p.radius * std::sin (a0)),
                                float (z) };
        const float to[3] = { float (p.x + p.radius * std::cos (a1)), float (p.y + p.radius * std::sin (a1)),
                              float (z) };
        AppendPreviewSegment (from, to, rgba, b);
    }
}

} // namespace

void BuildPlantingSchemeScene (const SchemeResult& scheme, SchemeMarker marker, int highlight,
                               PlantingSchemeGeometry& out)
{
    out = {};
    std::vector<double> top (scheme.plants.size ());
    for (size_t i = 0; i < scheme.plants.size (); ++i) {
        const PlacedPlant& p = scheme.plants[i];
        const bool dim = highlight >= 0 && p.species != highlight;
        const uint32_t rgba = SchemeSpeciesRgba (p.species);
        if (marker == SchemeMarker::Schematic) {
            // A stem kSchematicDiameter wide, as tall as its habit's schematic
            // height, and the mature crown's outline on the ground.
            PlacedPlant stem = p;
            stem.radius = kSchematicDiameter / 2.0;
            top[i] = p.z + kLift + p.schematicHeight;
            AppendCylinder (stem, p.schematicHeight, Display (rgba, dim ? 60 : 230), out.crowns.translucent);
            AppendRim (stem, top[i] + 0.005, Display (rgba, dim ? 70 : 255), out.crowns.depthTested);
            AppendRim (p, p.z + kLift + 0.02, Display (rgba, dim ? 70 : 255), out.crowns.depthTested);
            continue;
        }
        const double h = marker == SchemeMarker::Mature ? std::max (p.height, 0.05)
                                                        : std::min (kSchemeDiscHeight, std::max (p.height, 0.05));
        top[i] = p.z + kLift + h;
        AppendCylinder (p, h, Display (rgba, dim ? 40 : 150), out.crowns.translucent);
        AppendRim (p, top[i] + 0.005, Display (rgba, dim ? 70 : 255), out.crowns.depthTested);
        // A small cross at the centre, where the species' line starts.
        const double t = std::min (0.15, p.radius * 0.3);
        const uint32_t ink = Display (rgba, dim ? 70 : 255);
        const float ax[3] = { float (p.x - t), float (p.y), float (top[i] + 0.01) };
        const float bx[3] = { float (p.x + t), float (p.y), float (top[i] + 0.01) };
        const float ay[3] = { float (p.x), float (p.y - t), float (top[i] + 0.01) };
        const float by[3] = { float (p.x), float (p.y + t), float (top[i] + 0.01) };
        AppendPreviewSegment (ax, bx, ink, out.links.depthTested);
        AppendPreviewSegment (ay, by, ink, out.links.depthTested);
    }
    for (const SchemeLink& link : scheme.links) {
        const PlacedPlant& a = scheme.plants[size_t (link.a)];
        const PlacedPlant& b = scheme.plants[size_t (link.b)];
        const bool dim = highlight >= 0 && a.species != highlight;
        // Lines run at the higher of the two tops, so neither cylinder swallows them.
        const double z = std::max (top[size_t (link.a)], top[size_t (link.b)]) + 0.01;
        const float from[3] = { float (a.x), float (a.y), float (z) };
        const float to[3] = { float (b.x), float (b.y), float (z) };
        AppendPreviewSegment (from, to, Display (SchemeSpeciesRgba (a.species), dim ? 70 : 255), out.links.depthTested);
    }
    out.plants = scheme.plants.size ();
}

void AppendEnvelopePreview (const SchemeSite& site, size_t row, const std::vector<SizeClass>& classes,
                            const SizeFitResult& fit, PlantingSchemeGeometry& out)
{
    if (row >= site.Size () || fit.reason.size () != site.Size () * classes.size ())
        return;
    for (size_t rank = 0; rank < fit.order.size (); ++rank) {
        const size_t c = size_t (fit.order[rank]);
        PlacedPlant p;
        p.x = site.x[row];
        p.y = site.y[row];
        p.z = site.groundZ[row];
        p.radius = classes[c].spread / 2.0;
        p.height = classes[c].height;
        const bool fits = fit.ReasonAt (row, c, classes.size ()) == SizeReason::Fits;
        const uint32_t rgba = fits ? SizeClassRgba (int (rank)) : kSizeNoneRgba;
        AppendCylinder (p, p.height, Display (rgba, 45), out.crowns.translucent);
        AppendRim (p, p.z + kLift + p.height, Display (rgba, 255), out.crowns.depthTested);
        AppendRim (p, p.z + kLift + 0.02, Display (rgba, 255), out.crowns.depthTested);
    }
}

void AppendSizeFitMap (const SchemeSite& site, const SizeFitResult& fit, PlantingSchemeGeometry& out)
{
    // OPAQUE, IN THE COPLANAR DECAL BUCKET: a translucent square mixed the
    // class colour with the light map under it, and its edge with neighbours.
    GhPreviewBucket& b = out.crowns.surfaceDecals;
    const size_t n = std::min (site.Size (), fit.largest.size ());
    for (size_t s = 0; s < n; ++s) {
        uint32_t rgba = kSizeNoneRgba;
        if (fit.largest[s] >= 0) {
            const auto at = std::find (fit.order.begin (), fit.order.end (), int (fit.largest[s]));
            rgba = SizeClassRgba (int (at - fit.order.begin ()));
        }
        else if (fit.anyUnknown[s])
            rgba = kSizeUnknownRgba;
        const uint32_t ink = Display (rgba, 255);
        // The whole cell, so neighbours meet without a seam of the map below.
        const double half =
            (site.area.size () == site.Size () && site.area[s] > 0 ? std::sqrt (site.area[s]) : 0.5) * 0.5;
        const double z = site.groundZ[s] + 0.03;
        const uint32_t base = uint32_t (b.meshVertices.size ());
        Vertex (b, site.x[s] - half, site.y[s] - half, z, 0, 0, 1, ink);
        Vertex (b, site.x[s] + half, site.y[s] - half, z, 0, 0, 1, ink);
        Vertex (b, site.x[s] + half, site.y[s] + half, z, 0, 0, 1, ink);
        Vertex (b, site.x[s] - half, site.y[s] + half, z, 0, 0, 1, ink);
        b.meshIndices.insert (b.meshIndices.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

} // namespace geomsrv::archviz::planting
