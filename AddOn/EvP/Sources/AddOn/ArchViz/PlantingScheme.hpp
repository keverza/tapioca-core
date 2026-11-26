#ifndef TAPIOCA_ARCHVIZ_PLANTINGSCHEME_HPP
#define TAPIOCA_ARCHVIZ_PLANTINGSCHEME_HPP

// ArchViz/PlantingScheme -- a mixed planting placed on a planting layer (#278):
// the species the user picks, how many of each, where they go and the
// community warnings for the mix.
//
// Pure: no device, no ImGui, no DevKit. The region is a measured planting
// layer's samples (plan position, host surface height, light class, plan area,
// host), so a plant is only placed where the layer was measured.
//
// ⚠️ A PROPOSAL, NOT A PLAN. Placement is a deterministic spread that keeps
// mature crowns apart and puts each plant on a light class it accepts and,
// when the site's room was measured (SunStudyCrownClearance), where its mature
// crown clears overhangs and walls. It does not check roots, utilities, paths
// or the standard's own spacing rules, and nothing is written to the model.
//
// ⚠️ ABOVE GROUND ONLY. The space fit is a cylinder of the mature crown radius
// and height against the study's own geometry. Rooting is never assessed, and
// the panel says so.
//
// ⚠️ UNKNOWN IS NOT A PASS. A plant without light classes is placed on any
// measured sample and reported as unchecked; a missing spread uses the density's
// footprint, else a stated 0.25 m, and says so.
//
// ⚠️ COMPATIBILITY IS SITE-FIRST (research note "plant compat"): a shared site
// envelope (light, water, ground) before competition. A water mismatch is a
// different-zone problem, not plant-to-plant conflict, and is worded so.

#include "ArchViz/PlantCatalogue.hpp"
#include "ArchViz/PlantingZones.hpp"
#include "SunStudy/SunStudyCrownClearance.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

struct PlantingHud;

// How the viewer draws a placed plant (PlantingSchemeScene).
enum class SchemeMarker : uint8_t {
    Disc,  // a low cylinder, kSchemeDiscHeight tall (or the plant's height, if lower)
    Mature // the plant's mature height
};

// One species in the mix. `count` 0 is "as many as fit".
struct SchemeSpecies {
    std::string plantId;
    int count = 1;
};

// The region: one planting layer's samples, parallel arrays.
struct SchemeSite {
    std::vector<double> x, y;    // plan position, m
    std::vector<double> groundZ; // host surface under the sample, m
    std::vector<int> cls;        // the tab's class (ClassOf), -1 unavailable
    std::vector<uint32_t> host;  // host index, for the notes
    float layerHeight = 0;
    // The room above each sample; empty when not measured (space not checked).
    evp::sunstudy::CrownClearance clearance;
    size_t Size () const
    {
        return x.size ();
    }
};

struct PlacedPlant {
    int species = -1; // index into the scheme's species
    double x = 0, y = 0, z = 0;
    double radius = 0; // mature crown radius used for spacing, m
    double height = 0; // mature height used for the marker, m
    int cls = -1;
    bool lightChecked = false; // the plant has light classes
    bool spaceChecked = false; // the site's room was measured
};

struct SchemeLink {
    int a = -1, b = -1; // indices into SchemeResult::plants, same species
};

enum class SchemeSeverity : uint8_t { Info, Warning, Conflict };

struct SchemeNote {
    SchemeSeverity severity = SchemeSeverity::Info;
    std::string text;
    std::vector<int> species; // the species it is about
};

struct SchemeSpeciesSize {
    double radius = 0.25, height = 0.5;
    std::string radiusFrom, heightFrom; // "spread", "density", "assumed", "height", "layer"
};

// Why a sample does or does not hold a crown, above ground.
enum class SpaceFit : uint8_t {
    Fits,
    Overhead, // something above is lower than the crown's height and the margin
    Narrow,   // a wall or other surface is closer than the radius and the margin, at some level the crown reaches
    Beyond    // the crown is wider than the measured reach: not known
};
const char* SpaceFitName (SpaceFit fit);

// The fit of a crown `radius` wide and `height` tall at `sample`, with
// `margin` m kept clear on every side and above. Levels at or below the height
// are checked, and always the lowest.
SpaceFit FitAt (const evp::sunstudy::CrownClearance& room, size_t sample, double radius, double height, double margin);

// Per species, how the samples on its accepted light classes fared.
struct SchemeRoom {
    size_t lightSamples = 0; // on an accepted light class
    size_t fits = 0, overhead = 0, narrow = 0, beyond = 0;
};

struct SchemeResult {
    std::vector<PlacedPlant> plants;
    std::vector<SchemeLink> links;
    std::vector<int> wanted, placed; // per species; wanted 0 = as many as fit
    std::vector<SchemeSpeciesSize> sizes;
    std::vector<SchemeRoom> room; // per species; empty when the space was not checked
    std::vector<SchemeNote> notes;
    bool spaceChecked = false;
};

struct SchemeOptions {
    // Crowns may overlap by this fraction of the smaller radius (0 = touching).
    double crownOverlap = 0.0;
    int maxPlants = 400; // all species together; a cap, reported when reached
    // Kept clear around and above each mature crown, m, when the room is known.
    double clearanceMargin = 0.0;
    // False: ignore a measured room (the "Clear of walls and overhangs" switch).
    bool checkSpace = true;
};

// The crown radius and marker height a plant is placed with, and where they
// came from: the larger stated spread (conservative), the density's footprint
// sqrt(1 / (pi * density)), else 0.25 m; the larger stated height, else the
// plant's receiver height.
SchemeSpeciesSize SizeOf (const Plant& plant);

// Places every species, largest crown first, each plant at the sample farthest
// from the crowns already placed among those its light classes accept, never
// closer than the two radii allow. Then links each species' plants by a
// minimum spanning tree in plan and adds the community notes. Deterministic.
SchemeResult PlaceScheme (const PlantingHud& hud, const Catalogue& catalogue, const std::vector<SchemeSpecies>& species,
                          const SchemeSite& site, const SchemeOptions& options = {});

// The community notes alone: water and ground overlap, growth intensity and
// trait competition, regulatory status, missing data. No placement needed.
std::vector<SchemeNote> CheckCommunity (const Catalogue& catalogue, const std::vector<SchemeSpecies>& species);

// The site's samples inside `zone`, in their order.
SchemeSite RestrictToZone (const SchemeSite& site, const PlantingZone& zone);

// The catalogue index of a scheme species, -1 when it is gone.
int FindPlant (const Catalogue& catalogue, const std::string& id);

// A distinct display colour per species, RGBA8 0xRRGGBBAA.
uint32_t SchemeSpeciesRgba (int species);

} // namespace geomsrv::archviz::planting

#endif
