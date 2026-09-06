#ifndef EVP_PROJECTENV_PROJECTSUN_HPP
#define EVP_PROJECTENV_PROJECTSUN_HPP

// WHICH SUN ARCHICAD'S 3D WINDOW IS ACTUALLY SHADING WITH.
//
// ⚠️ ONE IMPLEMENTATION, AND IT WAS EXPENSIVE. This is PLAT-RE67, confirmed
// live on 2026-08-14: a project carries TWO independent suns, and the obvious
// one is the wrong one.
//
//   * API_PlaceInfo          - Project Location's sun. What GetPlaceSets answers.
//   * API_SunAngleSettings   - 3D Projection Settings -> Sun Position, living
//                              inside API_PerspPars/API_AxonoPars.
//
// THE 3D WINDOW SHADES WITH THE SECOND ONE, and the two drift apart in silence.
// The project that found it had the place at 2018-08-20 12:00 (altitude 47.5
// deg) and the view at 2017-03-22 10:00 (28.4 deg) - a 19-degree error that
// looked like a convention bug for weeks.
//
// It has now been got wrong twice, in two different ways, which is why it is a
// function rather than a paragraph of advice:
//
//   * reading the PLACE's cached angles freezes the sun, because GetPlaceSets
//     returns what was last WRITTEN and the Sun dialog does not necessarily
//     recompute it (four cameras captured at four times of day, all identical);
//   * recomputing the PLACE unconditionally throws away a sun the user TYPED,
//     which is the failure ExtractionEnvironment.cpp was originally written up
//     for.
//
// Both are avoided by asking sunPosOpt which mode the VIEW is in, and by
// evaluating a Date-and-Time view against ITS OWN moment rather than the
// project's. Neither caller re-derives any of that.
//
// ⚠️ DELIBERATELY NOT UNDER ArchViz/, and the SunStudy tier entry in
// tools/quality/check_cpp.py already makes this argument for the same reason:
// the Diligent viewport is ONE consumer and the graph's camera node is another,
// so a features-tier home would put a shared answer behind one of its own
// clients - and the architecture gate would refuse the graph runtime the
// include, which is exactly what it did.
//
// MAIN THREAD ONLY: this is ACAPI.

#include <string>

namespace geomsrv {

struct ProjectSun {
    // False only when the project place could not be read at all.
    bool valid = false;

    // ⚠️ RADIANS, AND THE CONVENTION IS API_PlaceInfo'S: sunAngXY is a
    // MATHEMATICAL angle, counterclockwise from the model's +X axis, already in
    // model space; sunAngZ is the altitude above the horizon. Settled against an
    // independent NOAA calculation, twice, at two different project-north values
    // (NativeCommands/ProjectCommands.cpp). The compass bearing a person reads
    // is `north - sunAngXY` - NOT `90 - sunAngXY`, which is correct only at the
    // default north and has already fooled one round of this work.
    double sunAngXY = 0.0;
    double sunAngZ = 0.0;

    // Radians, CCW from +X, as the DevKit gives it.
    double north = 0.0;

    // True when the 3D projection's own Sun Position was used - the ordinary
    // case. False means the place sun was the fallback, and shadows will not
    // match Archicad's unless the two happen to agree.
    bool fromView = false;

    // Where it came from, in words, for a log or a HUD. "The viewer's sun is
    // wrong" and "the viewer is reading a different sun than the 3D window" are
    // one symptom until this can be read.
    std::string source;
};

// Reads the place, then the 3D projection's own sun, and returns whichever
// Archicad is really shading with.
ProjectSun ResolveProjectSun ();

} // namespace geomsrv

#endif
