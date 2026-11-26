// The Planting tab's area table (#278): ReadPlantingAreas, declared with the
// runner's other plain-value calls in SunStudyHudRunner.hpp. Reads the store
// and the kept period; traces nothing.

#include "NativeCommands/SunStudyHudPeriod.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"
#include "SunStudy/SunStudyPlantingAreas.hpp"
#include "SunStudy/SunStudyStore.hpp"

namespace geomsrv::sunstudyhud {

bool ReadPlantingAreas (const std::string& studyId, const std::vector<double>& edges, PlantingAreas& out,
                        std::string& error)
{
    using evp::sunstudy::SunStudyStore;
    out = PlantingAreas ();
    out.studyId = studyId;
    error.clear ();
    std::vector<double> hours;
    out.period = period::MeanDailyHours (studyId, hours, error);
    if (!out.period) {
        if (!error.empty ())
            return false;
        std::vector<double> positions, normals;
        if (!SunStudyStore::Get ().Results (studyId, hours, positions, normals, nullptr, error, false))
            return false;
    }
    SunStudyStore::SampleCells cells;
    if (!SunStudyStore::Get ().Cells (studyId, cells, error))
        return false;
    if (cells.groups.size () != hours.size ()) {
        error = "the study's cells do not match its hours";
        return false;
    }
    if (!evp::sunstudy::BuildPlantingAreaTable (hours, cells.areas, cells.groups, cells.unavailableByMesh, edges,
                                                out.table, error))
        return false;
    for (const auto& row : out.table.hosts)
        out.hostGuids.push_back (row.group < cells.meshGuids.size () ? cells.meshGuids[row.group] : std::string ());
    return true;
}

bool ReadPlantingSite (const std::string& studyId, double layerHeight, PlantingSite& out, std::string& error)
{
    using evp::sunstudy::SunStudyStore;
    out = PlantingSite ();
    out.studyId = studyId;
    error.clear ();
    std::vector<double> hours, positions, normals;
    if (!SunStudyStore::Get ().Results (studyId, hours, positions, normals, nullptr, error, true))
        return false;
    std::vector<double> mean;
    if (period::MeanDailyHours (studyId, mean, error))
        hours = std::move (mean);
    else if (!error.empty ())
        return false;
    SunStudyStore::SampleCells cells;
    if (!SunStudyStore::Get ().Cells (studyId, cells, error))
        return false;
    if (positions.size () != hours.size () * 3 || cells.groups.size () != hours.size ()) {
        error = "the study's samples, positions and cells do not match";
        return false;
    }
    const size_t n = hours.size ();
    out.x.resize (n);
    out.y.resize (n);
    out.groundZ.resize (n);
    for (size_t i = 0; i < n; ++i) {
        out.x[i] = positions[3 * i];
        out.y[i] = positions[3 * i + 1];
        out.groundZ[i] = positions[3 * i + 2] - layerHeight;
    }
    out.hours = std::move (hours);
    out.groups = std::move (cells.groups);
    out.meshGuids = std::move (cells.meshGuids);
    return true;
}

} // namespace geomsrv::sunstudyhud
