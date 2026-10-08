#ifndef EVP_ARCHVIZ_VISIBILITYSTUDYCONTROLLER_HPP
#define EVP_ARCHVIZ_VISIBILITYSTUDYCONTROLLER_HPP

#include "Geometry/Mesh.hpp"
#include "SunStudy/VisibilityStudy.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz::visibilitystudy {

struct Request {
    std::shared_ptr<const Snapshot> snapshot;
    std::vector<std::string> fromElements;
    std::vector<std::string> toElements;
    evp::sunstudy::VisibilityStudyOptions options;
};

struct Status {
    bool running = false;
    bool complete = false;
    std::string studyId;
    std::string error;
    uint64_t snapshotId = 0;
    size_t sampleCount = 0;
    size_t aimPointCount = 0;
    size_t rayCount = 0;
    size_t visibleSamples = 0;
    double meanVisibility = 0.0;
    double analysisMilliseconds = 0.0;
};

// RENDER THREAD submission; calculation and atlas assembly run on a worker.
bool Submit (Request request, std::string& error);
Status GetStatus ();
void Cancel ();
void ClearDisplay ();
// Called before add-on teardown; never leave a worker to static destruction.
void Shutdown ();

} // namespace geomsrv::archviz::visibilitystudy

#endif
