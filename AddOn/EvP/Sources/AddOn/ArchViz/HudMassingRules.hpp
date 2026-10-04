#ifndef EVP_ARCHVIZ_HUDMASSINGRULES_HPP
#define EVP_ARCHVIZ_HUDMASSINGRULES_HPP

#include "ArchViz/MassingRules.hpp"
#include "ArchViz/MassingCalculation.hpp"

namespace geomsrv::archviz::hudmassingrules {
struct Draft {
    massingrules::Page source;
    std::vector<massingrules::Assignment> assignments;
    std::vector<bool> endpoints; // run-local only; never written to element metadata
    std::vector<bool> regulated;
    massingcalculation::Request calculation;
    std::vector<massingcalculation::Request> calculations;
    std::optional<massingcalculation::Request> lastRequested;
    int selected = 0;
    int targetPoint = -1;
    int targetEdge = -1;
    double defaultDistance = 3;
    bool dirty = false;
    std::string note;
};
void Sync (const massingrules::Page& page, Draft& draft);
// Returns explicit Save requests only. Layout does not touch ACAPI or storage.
std::vector<massingrules::Edit> Draw (const massingrules::Page& page, Draft& draft, bool busy = false,
                                      const std::string& calculationNote = {},
                                      const std::shared_ptr<const massingcalculation::Preview>& preview = {});
} // namespace geomsrv::archviz::hudmassingrules
#endif
