#ifndef GEOMETRYSERVER_SCREENSHOTCAPTURE_HPP
#define GEOMETRYSERVER_SCREENSHOTCAPTURE_HPP

#include <string>

// Native Archicad screenshots of the 3D window, using its live shading style
// (vectorial/OpenGL), NOT the photorender engine. ACAPI — MAIN THREAD ONLY.
// Both functions require the current window to be the 3D model window.
namespace geomsrv {

// Save the current 3D view exactly as displayed. Fills pngBytes on success.
bool CaptureCurrentView (std::string& pngBytes, std::string& err);

// Temporarily switch the 3D projection to a straight-down top view (parallel),
// capture, then restore the previous projection. Fills pngBytes on success.
bool CaptureTopDown (std::string& pngBytes, std::string& err);

} // namespace geomsrv

#endif
