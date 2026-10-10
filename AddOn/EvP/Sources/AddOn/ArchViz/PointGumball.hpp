#ifndef EVP_ARCHVIZ_POINTGUMBALL_HPP
#define EVP_ARCHVIZ_POINTGUMBALL_HPP

namespace geomsrv::archviz {

// Parameters in world metres, never a screen delta guessed as a model move.
// Axis 0..2 is constrained translation; 3 is the point's horizontal XY plane.
bool PointGumballParameter (const double point[3], int axis, const double rayOrigin[3], const double rayDirection[3],
                            double parameter[3]);

} // namespace geomsrv::archviz
#endif
