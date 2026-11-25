#include "SunStudy/SunStudyPlantingAreas.hpp"
#include "SunStudy/SunStudyLightClass.hpp"

#include <algorithm>
#include <map>

namespace evp::sunstudy {

namespace {

bool Classify (const std::vector<double>& hours, const std::vector<double>& areas, const std::vector<double>& edges,
               PlantingAreaRow& row, std::string& error)
{
    LightClassAreas classes;
    if (!ClassifyLightAreas (hours, areas, edges, classes, error))
        return false;
    row.classAreas = std::move (classes.areas);
    row.evaluatedArea = classes.evaluatedArea;
    row.samples = hours.size ();
    row.unclassified = classes.unclassified;
    return true;
}

void AddLoss (PlantingAreaRow& row, const ReceptorHostLoss& loss)
{
    row.steepArea += loss.steepArea;
    row.blockedArea += loss.blockedArea;
    row.invalidArea += loss.invalidArea;
}

} // namespace

bool BuildPlantingAreaTable (const std::vector<double>& hours, const std::vector<double>& areas,
                             const std::vector<uint32_t>& groups, const std::vector<ReceptorHostLoss>& losses,
                             const std::vector<double>& edges, PlantingAreaTable& out, std::string& error)
{
    out = PlantingAreaTable ();
    if (hours.size () != areas.size () || hours.size () != groups.size ()) {
        error = "hours, areas and groups differ in length";
        return false;
    }
    if (!Classify (hours, areas, edges, out.total, error))
        return false;
    struct Split {
        std::vector<double> hours, areas;
    };
    std::map<uint32_t, Split> byGroup;
    for (size_t i = 0; i < hours.size (); ++i) {
        Split& split = byGroup[groups[i]];
        split.hours.push_back (hours[i]);
        split.areas.push_back (areas[i]);
    }
    for (const ReceptorHostLoss& loss : losses)
        byGroup.try_emplace (loss.group);
    for (const auto& [group, split] : byGroup) {
        PlantingAreaRow row;
        row.group = group;
        if (!Classify (split.hours, split.areas, edges, row, error))
            return false;
        out.hosts.push_back (std::move (row));
    }
    for (const ReceptorHostLoss& loss : losses) {
        AddLoss (out.total, loss);
        const auto at = std::lower_bound (out.hosts.begin (), out.hosts.end (), loss.group,
                                          [] (const PlantingAreaRow& row, uint32_t g) { return row.group < g; });
        if (at != out.hosts.end () && at->group == loss.group)
            AddLoss (*at, loss);
    }
    return true;
}

} // namespace evp::sunstudy
