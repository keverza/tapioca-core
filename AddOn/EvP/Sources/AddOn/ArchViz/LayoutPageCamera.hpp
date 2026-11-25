#pragma once

#include "ArchViz/Camera.hpp"
#include "ArchViz/DiligentHudCaptureFrame.hpp"

namespace geomsrv::archviz::viewerhud {
inline bool SwitchPageCamera (CaptureFrame& frame, Camera& live)
{
    if (frame.cameraPage == frame.reportType)
        return false;
    if (frame.cameraPage < 0) {
        for (auto& page : frame.pageCameras)
            page = std::make_shared<Camera> (live);
    }
    else
        *frame.pageCameras[frame.cameraPage] = live;
    frame.cameraPage = frame.reportType;
    live = *frame.pageCameras[frame.cameraPage];
    return true;
}
} // namespace geomsrv::archviz::viewerhud
