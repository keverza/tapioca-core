#ifndef EVP_ARCHVIZ_ROOFSLOPEHUD_HPP
#define EVP_ARCHVIZ_ROOFSLOPEHUD_HPP
#include "ArchViz/RoofSlope.hpp"
#include "ArchViz/RoofSlopeCanvas.hpp"
#include <optional>
namespace geomsrv::archviz::overlayhud {
struct Input;
}
namespace geomsrv::archviz::roofhud {
struct NumberEdit {
    uint64_t revision = 0;
    int node = -1, cell = -1, side = -1;
    double value = 0;
    std::string label;
    uint64_t sessionToken = 0;
};
struct Session {
    roofslope::Editor editor;
    bool previousLock = false, close = false, footprint = false;
    uint64_t token = 0;
    int drag = 0, corner = -1, ring = -1, vertex = -1;
    roofslope::Vec anchor, last;
    double startZ = 0;
    float startX = 0, startY = 0;
    int axis = -1;
    bool dragPlanar = false;
    roofslope::Snap snap;
    std::optional<NumberEdit> number;
    roofcanvas::Frame canvas;
    char numberText[64] = {};
    bool focusNumber = false;
};
// Inverse of the supplied view projection at a fixed Z, with a singularity refusal.
// No camera guesses, ACAPI calls or millimetres-per-pixel constants.
bool OntoPlane (const overlayhud::Input& view, const roofslope::Data& data, float px, float py, double z,
                roofslope::Vec seed, roofslope::Vec& local);
void OnView (Session& session, const overlayhud::Input& view, bool hovered, float scale);
void Panel (Session& session);
bool Answer (Session& session, const NumberEdit& edit, double value);
bool DeleteSelection (Session& session);
} // namespace geomsrv::archviz::roofhud
#endif
