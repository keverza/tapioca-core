#ifndef EVP_ARCHVIZ_HUDMASSINGSTATS_HPP
#define EVP_ARCHVIZ_HUDMASSINGSTATS_HPP
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/HudShell.hpp"

namespace geomsrv::archviz::hudmassingstats {
bool CoefficientInputs (massingareas::Coefficients& coefficients, std::vector<massingareas::NumberEdit>& numbers);
// Pure layout. The owner publishes temporary geometry after the frame.
std::string Draw (const massingslices::Result& result, const overlaylayers::Panel& look = hudshell::PlainLook (),
                  float scale = 1);
} // namespace geomsrv::archviz::hudmassingstats
#endif
