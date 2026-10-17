#ifndef EVP_SURFACETAKEOFFLAYER_HPP
#define EVP_SURFACETAKEOFFLAYER_HPP

// OVERLAY-INVARIANTS.md: a client of existing layers, never a camera/renderer.
#include "ArchViz/OverlayLayers.hpp"
#include "Geometry/SurfaceAttributeMap.hpp"
#include "Geometry/SurfaceCriteria.hpp"

namespace geomsrv::archviz::surfacetakeoff {
constexpr const char* kLayer = "tapioca.exposedSurfaces";
constexpr size_t kMaxSurfaces = 64;
uint32_t DefaultColor (size_t index);
struct SurfaceRow {
    int32_t index = 0;
    std::string name;
    uint32_t rgba = 0;
    double areaM2 = 0;
};
enum class Facing { Top, Bottom, Side }; // up/down within 45 degrees of world Z; remainder side
struct Presentation {
    overlaylayers::Layer layer;
    std::vector<SurfaceRow> rows;
    size_t triangles = 0;
    double areaM2 = 0;
    std::vector<int32_t> meshSurfaces;
    std::vector<double> meshAreas;
    std::vector<Facing> meshFacings;
    std::vector<std::string> meshElements;
    std::map<std::string, surfacecriteria::Element> elements;
    std::map<int32_t, double> nativeAreas;
    std::string scopeLabel, coverLabel, comparisonNote;
    double contactToleranceM = 0;
};
// Pure UI state over an already verified capture. Applying a material list or
// highlighting never changes the stored per-material areas or recalculates geometry.
struct UiState {
    std::string token;
    std::string warning;
    std::set<int32_t> pending, selected;
    std::optional<int32_t> focus;
    bool includeTop = true, includeBottom = true, includeSides = true;
    std::vector<surfacecriteria::Rule> draftRules, rules;
    int32_t nextRuleId = 1;
    std::string criteriaError;
};
UiState InitialUi (const Presentation& presentation, const std::string& token);
std::string UiId (const UiState& state, const char* action, int32_t surface = 0);
bool ApplyUiChange (const Presentation& presentation, UiState& state, const std::string& kind, const std::string& id,
                    double value, const std::string& text = {});
overlaylayers::Layer UiLayer (const Presentation& presentation, const UiState& state);
std::map<int32_t, double> UiAreas (const Presentation& presentation, const UiState& state);
std::vector<std::string> CriterionValues (const Presentation& presentation, surfacecriteria::Field field);
bool ApplyCriteriaChange (const Presentation& presentation, UiState& state, const std::string& kind,
                          const std::string& id, double value, const std::string& text);
bool MatchesCriteria (const Presentation& presentation, const UiState& state, size_t mesh);
overlaylayers::Panel CriteriaPanel (const Presentation& presentation, const UiState& state);
// Pure: the exact selected fragment triangles are copied to the layer and summed
// independently for its table. Whole-element native quantities are a comparison,
// never a replacement for highlighted area or schedule-equivalence evidence.
bool Build (const Snapshot& snapshot, const exposed::Result& result, const exposed::SurfaceSelection& selection,
            const std::vector<int32_t>& surfaces, const std::map<int32_t, double>& nativeAreas,
            const std::string& comparisonNote, Presentation& output, std::string& error,
            const std::vector<uint32_t>& colors = {}, bool allWalls = false);
} // namespace geomsrv::archviz::surfacetakeoff
#endif
