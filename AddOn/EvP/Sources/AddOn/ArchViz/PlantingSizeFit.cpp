#include "ArchViz/PlantingSizeFit.hpp"

#include "ArchViz/PlantingHudModel.hpp"

#include <algorithm>
#include <numeric>

namespace geomsrv::archviz::planting {

std::vector<SizeClass> DefaultSizeClasses ()
{
    SizeClass small, medium, large;
    small.name = "Small";
    small.spread = 1.5f;
    small.height = 1.5f;
    medium.name = "Medium";
    medium.spread = 4.0f;
    medium.height = 6.0f;
    large.name = "Large";
    large.spread = 10.0f;
    large.height = 15.0f;
    return { small, medium, large };
}

const char* SizeReasonName (SizeReason r)
{
    switch (r) {
        case SizeReason::Fits:
            return "fits";
        case SizeReason::Light:
            return "light";
        case SizeReason::NoGo:
            return "no-go zone";
        case SizeReason::Overhead:
            return "overhang";
        case SizeReason::Narrow:
            return "wall";
        case SizeReason::Unknown:
            return "not known";
        case SizeReason::Off:
            return "off";
    }
    return "";
}

uint32_t SizeClassRgba (int rank)
{
    static constexpr uint32_t kGreens[] = { 0x5F9B72FFu, 0x86B98FFFu, 0xB1D5A3FFu, 0xD8EBC6FFu };
    constexpr int n = int (sizeof (kGreens) / sizeof (kGreens[0]));
    return kGreens[std::clamp (rank, 0, n - 1)];
}

SizeFitResult EvaluateSizeClasses (const PlantingHud& hud, const SchemeSite& site,
                                   const std::vector<SizeClass>& classes, double margin)
{
    SizeFitResult out;
    const size_t n = site.Size (), k = classes.size ();
    for (size_t c = 0; c < k; ++c)
        if (classes[c].on)
            out.order.push_back (int (c));
    std::stable_sort (out.order.begin (), out.order.end (), [&] (int a, int b) {
        const SizeClass &ca = classes[size_t (a)], &cb = classes[size_t (b)];
        return ca.spread != cb.spread ? ca.spread > cb.spread : ca.height > cb.height;
    });
    const bool space = !site.clearance.Empty () && site.clearance.Count () == n;
    const bool areas = site.area.size () == n;
    out.spaceChecked = space;
    out.largest.assign (n, -1);
    out.anyUnknown.assign (n, 0);
    out.reason.assign (n * k, uint8_t (SizeReason::Off));
    out.area.assign (k, {});
    for (size_t s = 0; s < n; ++s) {
        const double weight = areas ? site.area[s] : 1.0;
        out.measured += weight;
        const int cls = site.cls[s];
        const int light = cls >= 0 ? int (LightOfClass (hud, cls)) : -1;
        for (size_t c = 0; c < k; ++c) {
            const SizeClass& sc = classes[c];
            SizeReason r = SizeReason::Off;
            if (sc.on) {
                const double radius = sc.spread / 2.0, height = sc.height;
                if (light < 0 || !sc.light[size_t (light)])
                    r = SizeReason::Light;
                else if (KeptOut (site, s, radius, height, margin))
                    r = SizeReason::NoGo;
                else if (!space)
                    r = SizeReason::Unknown;
                else
                    switch (FitAt (site.clearance, s, radius, height, margin)) {
                        case SpaceFit::Fits:
                            r = SizeReason::Fits;
                            break;
                        case SpaceFit::Overhead:
                            r = SizeReason::Overhead;
                            break;
                        case SpaceFit::Narrow:
                            r = SizeReason::Narrow;
                            break;
                        case SpaceFit::Beyond:
                            r = SizeReason::Unknown;
                            break;
                    }
            }
            out.reason[s * k + c] = uint8_t (r);
            out.area[c][size_t (r)] += weight;
        }
        for (int c : out.order) {
            const SizeReason r = SizeReason (out.reason[s * k + size_t (c)]);
            if (r == SizeReason::Fits) {
                out.largest[s] = int8_t (c);
                break;
            }
            if (r == SizeReason::Unknown)
                out.anyUnknown[s] = 1;
        }
        if (out.largest[s] >= 0)
            out.anyUnknown[s] = 0;
    }
    return out;
}

} // namespace geomsrv::archviz::planting
