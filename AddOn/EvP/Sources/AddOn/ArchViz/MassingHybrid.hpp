#ifndef EVP_ARCHVIZ_MASSINGHYBRID_HPP
#define EVP_ARCHVIZ_MASSINGHYBRID_HPP
#include "ArchViz/MassingCalculation.hpp"
#include <memory>

namespace geomsrv::archviz::massinghybrid {
struct Page {
    bool busy = false, calculated = false;
    std::string note;
    std::shared_ptr<const massingcalculation::Preview> preview;
    std::shared_ptr<const geomsrv::Mesh> terrain; // Same current source/stamp as this adopted preview.
};
// MAIN THREAD facade. Requests are posted out of layout; immutable geometry is
// passed to Python on one owned worker. Poll adopts only current completions
// outside layout; Read is a cheap immutable view for the HUD's page preparation.
void Request (massingcalculation::Request request);
void Poll ();
Page Read ();
void Dimensions (bool shown);
void Forget ();
void Shutdown ();
} // namespace geomsrv::archviz::massinghybrid
#endif
