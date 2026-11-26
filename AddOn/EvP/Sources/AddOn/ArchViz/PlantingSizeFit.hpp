#ifndef TAPIOCA_ARCHVIZ_PLANTINGSIZEFIT_HPP
#define TAPIOCA_ARCHVIZ_PLANTINGSIZEFIT_HPP

// ArchViz/PlantingSizeFit -- "what size of plant fits here?" (#278 Stage 2):
// editable size classes (a mature crown spread and height, and the light
// classes they accept) checked at every sample of a planting layer, and the
// largest class that fits each sample.
//
// Pure: the Scheme's site (light class, measured room, no-go zones), no
// device, no ImGui.
//
// ⚠️ EVERY CLASS IS CHECKED ON ITS OWN. Classes are not assumed nested: a tall
// class may fit under open sky where a wide, lower one meets a wall. The
// largest is the largest class that passes, ranked by spread then height.
//
// ⚠️ A LIGHT-ONLY MATCH IS NOT A FIT. Where the room was not measured, or the
// crown is wider than the measured reach, the class is Unknown, never Fits.
//
// ⚠️ ABOVE GROUND ONLY. Rooting, soil depth and utilities are not assessed;
// the panel says so. The default classes are proposed templates, not data
// from the catalogue; edit them to the project.

#include "ArchViz/PlantCatalogue.hpp"
#include "ArchViz/PlantingScheme.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

struct PlantingHud;

struct SizeClass {
    std::string name;
    float spread = 1.5f;                                           // mature crown diameter, m
    float height = 1.5f;                                           // mature height, m
    std::array<bool, kLightClassCount> light { true, true, true }; // Shade, Partial, Full accepted
    bool on = true;
};

// Small (shrub, 1.5 m across, 1.5 m tall), Medium (small tree, 4 m, 6 m),
// Large (tree, 10 m, 15 m); every light class accepted. Proposed, editable.
std::vector<SizeClass> DefaultSizeClasses ();

enum class SizeReason : uint8_t {
    Fits,
    Light,    // the sample's light class is not one the class accepts
    NoGo,     // its crown would reach into a no-go zone
    Overhead, // something above is lower than its height
    Narrow,   // a wall is closer than its radius at a level it reaches
    Unknown,  // the room was not measured here, or the crown is wider than the reach
    Off       // the class is switched off
};
constexpr int kSizeReasonCount = 7;
const char* SizeReasonName (SizeReason r);

struct SizeFitResult {
    std::vector<int> order;          // switched-on classes, largest first
    std::vector<int8_t> largest;     // per sample: the largest class that fits, -1 none
    std::vector<uint8_t> anyUnknown; // per sample: 1 when no class fits but one is Unknown
    std::vector<uint8_t> reason;     // per sample x class (sample-major), SizeReason
    // Per class, plan area by reason (m²; sample counts when the site has no areas).
    std::vector<std::array<double, kSizeReasonCount>> area;
    double measured = 0.0;
    bool spaceChecked = false;

    SizeReason ReasonAt (size_t sample, size_t cls, size_t classes) const
    {
        return SizeReason (reason[sample * classes + cls]);
    }
};

SizeFitResult EvaluateSizeClasses (const PlantingHud& hud, const SchemeSite& site,
                                   const std::vector<SizeClass>& classes, double margin);

// A display colour per class rank (0 = largest), RGBA8 0xRRGGBBAA: greens,
// darker for the larger. And the colours for "nothing fits" and "not known".
uint32_t SizeClassRgba (int rank);
constexpr uint32_t kSizeNoneRgba = 0xB5544CFFu;
constexpr uint32_t kSizeUnknownRgba = 0x9A9A9AFFu;

} // namespace geomsrv::archviz::planting

#endif
