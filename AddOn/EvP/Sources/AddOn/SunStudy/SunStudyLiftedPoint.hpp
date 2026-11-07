#ifndef EVP_SUNSTUDY_SUNSTUDYLIFTEDPOINT_HPP
#define EVP_SUNSTUDY_SUNSTUDYLIFTEDPOINT_HPP

// SunStudy/SunStudyLiftedPoint -- a study point in the air (#152 follow-up).
//
// A person standing on a terrace is not a cell of the roof under them: the
// question is "is the sun visible from 1.8 m above this spot". The point sits on
// a scene surface and is lifted straight up by a chosen height, then traced as
// an omnidirectional receiver (no surface to face away from) against the
// study's own occluders and suns, so it measures the same scene the map did.
//
// ⚠️ IT NEVER STARTS INSIDE GEOMETRY. A ray that begins inside a slab or under a
// ceiling it then crosses reports a blocker that is not there: the analysis
// would glitch. The lift is clamped below whatever is overhead, with clearance,
// and the clamp is reported rather than hidden.
//
// Pure CPU geometry on an immutable, completed StudyRecord; no store lock.

#include "SunStudy/SunStudyPointProfile.hpp"

namespace evp::sunstudy {

struct StudyRecord;

constexpr double kLiftedPointClearance = 0.10; // metres kept below anything overhead

// The nearest analysis or context surface along a ray: where a dragged study
// point lands. False when the ray meets nothing the study can see.
bool SunStudySceneHit (const StudyRecord& record, const double origin[3], const double direction[3], double hit[3]);

struct LiftedPoint {
    double surface[3] = {};  // on the scene, as placed
    double position[3] = {}; // the receiver: surface + height up
    double requestedHeight = 0.0;
    double height = 0.0;  // as used, never more than requested
    bool clamped = false; // something overhead lowered it
};

// Lifts `surface` by `height` (metres, Z up), clamped below anything overhead.
bool LiftSunStudyPoint (const StudyRecord& record, const double surface[3], double height, LiftedPoint& lifted);

// The lifted receiver's day at the study's own steps: lit 0/1, facing 1, one
// "cell". The run's hour window travels with it.
bool LiftedPointDay (const StudyRecord& record, const LiftedPoint& lifted, PointDay& day);

} // namespace evp::sunstudy

#endif
