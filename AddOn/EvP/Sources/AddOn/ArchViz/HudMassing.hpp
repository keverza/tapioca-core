#ifndef EVP_ARCHVIZ_HUDMASSING_HPP
#define EVP_ARCHVIZ_HUDMASSING_HPP

// Pure Define model and native widgets. The owner captures viewport selection and
// writes roles after the frame; drawing never calls Archicad or runs Python.
#include <array>
#include <string>
#include <vector>
#include "ArchViz/MassingRules.hpp"
#include "ArchViz/MassingCalculation.hpp"
#include "ArchViz/MassingBuildings.hpp"

namespace geomsrv::archviz::hudmassing {

constexpr char kTabKey[] = "@massing";
enum class Group { PropertyLine, ExistingTerrain, NewTerrain, MassingSlabs };
enum class Action { Update, Add, Remove, Reselect, Clear };
struct Request {
    Group group = Group::PropertyLine;
    Action action = Action::Update;
};
struct Page {
    bool known = false;
    std::array<std::vector<std::string>, 4> guids;
    std::vector<massingbuildings::Record> buildingSlabs; // Model-wide identity index, including undefined siblings.
    std::string note;
    massingrules::Page rules;
    std::vector<massingrules::Page> parcels;
    bool calculationBusy = false;
    std::string calculationNote;
    std::string collapseNote;
    std::string inspectionNote;
    std::shared_ptr<const massingcalculation::Preview> preview;
};
struct Assignment {
    std::string guid;
    bool assign = false;
};
const char* Role (Group group);
const char* Label (Group group);
// Duplicates removed. Clear/Remove only clear this group's role; the adapter
// rechecks it before writing. Update is replacement, not an unbounded append.
std::vector<Assignment> Plan (Request request, const std::vector<std::string>& current,
                              const std::vector<std::string>& selected);
std::vector<Request> Draw (const Page& page);

} // namespace geomsrv::archviz::hudmassing
#endif
