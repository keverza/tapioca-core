#ifndef EVP_PROJECTENV_ARCHICADCAMERA_HPP
#define EVP_PROJECTENV_ARCHICADCAMERA_HPP

#include <string>

namespace geomsrv {

// Archicad's perspective 3D camera plus both renderer-ready and verbatim
// per-view sun values. The raw values exist solely for an exact restore.
struct ArchicadCamera {
    bool valid = false;
    std::string source;
    double eye[3] = { 0.0, 0.0, 0.0 };
    double target[3] = { 0.0, 0.0, 0.0 };
    double viewConeDegreesHorizontal = 0.0;

    bool hasSun = false;
    double sunAzimuthDegrees = 0.0;
    double sunAltitudeDegrees = 0.0;
    double sunBearingDegrees = 0.0;
    std::string sunSource;

    bool sunFromDate = false;
    double sunRawAzimuth = 0.0;
    double sunRawAltitude = 0.0;
    int sunYear = 0;
    int sunMonth = 0;
    int sunDay = 0;
    int sunHour = 0;
    int sunMinute = 0;
    int sunSecond = 0;
    bool sunSummerTime = false;
};

// MAIN THREAD ONLY. These are the single read/write implementation used by all
// camera-list clients. Restore switches to perspective and rebuilds when the 3D
// window is in front.
bool ReadArchicadCamera (ArchicadCamera& camera, std::string& error);
bool WriteArchicadCamera (const ArchicadCamera& camera, bool& threeDWindowInFront, std::string& error);

} // namespace geomsrv

#endif
