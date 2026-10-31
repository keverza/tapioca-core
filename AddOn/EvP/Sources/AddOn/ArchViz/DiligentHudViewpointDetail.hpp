#pragma once
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/ViewpointStudyController.hpp"

namespace geomsrv::archviz::viewerhud::viewpointdetail {
bool ShowsPointGumball (const HudState& state, const viewpointstudy::Settings& settings);
bool FaceTarget (const HudState& state, viewpointstudy::Settings& settings);
bool SnapUnderCursor (const double origin[3], const double direction[3], bool eye, double at[3]);
int DragAxis (int part);
bool LeftTransition (const InputSnapshot& input, bool down);
} // namespace geomsrv::archviz::viewerhud::viewpointdetail
