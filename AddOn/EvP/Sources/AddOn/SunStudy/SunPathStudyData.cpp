#include "SunStudy/SunPathStudyData.hpp"
#include "SunStudy/SunStudyStore.hpp"

namespace geomsrv::sunstudyhud {

bool ReadSunPathStudyDay (const std::string& studyId, SunPathStudyDay& day)
{
    // CompletedRecord returns an owning handle after releasing the store lock.
    // Inputs/series of that record are immutable, including during Represent.
    const auto record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (studyId);
    if (record == nullptr) {
        day = {};
        return false;
    }
    if (day.available && day.studyId == studyId && day.revision == record->storeRevision)
        return true;
    day.available = true;
    day.studyId = studyId;
    day.revision = record->storeRevision;
    day.year = record->year;
    day.month = record->month;
    day.day = record->day;
    day.hourFrom = record->hourFrom;
    day.hourTo = record->hourTo;
    day.timestepMinutes = record->timestepMinutes;
    day.steps = record->series.Steps ();
    return true;
}

} // namespace geomsrv::sunstudyhud
