#ifndef EVP_ARCHVIZ_HUDMASSINGSTATS_HPP
#define EVP_ARCHVIZ_HUDMASSINGSTATS_HPP
#include "ArchViz/MassingSlices.hpp"

namespace geomsrv::archviz::hudmassingstats {
// Pure layout. The owner publishes temporary geometry after the frame.
std::string Draw (const massingslices::Result& result);
} // namespace geomsrv::archviz::hudmassingstats
#endif
