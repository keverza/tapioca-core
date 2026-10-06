#ifndef EVP_ARCHVIZ_HUDMASSINGRULES_HPP
#define EVP_ARCHVIZ_HUDMASSINGRULES_HPP

#include "ArchViz/MassingRules.hpp"
#include "ArchViz/MassingCalculation.hpp"
#include <map>

namespace geomsrv::archviz::hudmassingrules {
struct NumberEdit {
    massingcalculation::Request before;
    std::string key;
    double number = 0, min = 0, max = 1000;
    int edge = -1;
};
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
    bool labels = true;
    bool offsetDimensions = false;
    std::vector<NumberEdit> numbers;
    std::string note;
    std::string pickedParcel;
};
struct SiteDraft {
    std::map<std::string, Draft> parcels;
    std::optional<massingcalculation::Request> lastRequested;
};
void SyncSite (const std::vector<massingrules::Page>& pages, Draft& active, SiteDraft& site);
massingcalculation::Request SiteInputs (const std::vector<massingrules::Page>& pages, const Draft& active,
                                        const SiteDraft& site);
std::vector<massingrules::Edit> DrawSite (const std::vector<massingrules::Page>& pages, Draft& active, SiteDraft& site,
                                          bool busy, const std::string& note,
                                          const std::shared_ptr<const massingcalculation::Preview>& preview);
void Sync (const massingrules::Page& page, Draft& draft);
bool HasUnsavedOffsets (const Draft& draft);
bool AnswerNumber (Draft& draft, const NumberEdit& edit, double number);
// 0/1/3 set a fixed offset; -1 opens a cancellable Custom number panel.
void SelectOffset (Draft& draft, int preset);
// Returns explicit Save requests only. Layout does not touch ACAPI or storage.
std::vector<massingrules::Edit> Draw (const massingrules::Page& page, Draft& draft, bool busy = false,
                                      const std::string& calculationNote = {},
                                      const std::shared_ptr<const massingcalculation::Preview>& preview = {},
                                      const std::vector<massingrules::Page>& parcels = {},
                                      const std::shared_ptr<const massingcalculation::Preview>& sitePreview = {});
} // namespace geomsrv::archviz::hudmassingrules
#endif
