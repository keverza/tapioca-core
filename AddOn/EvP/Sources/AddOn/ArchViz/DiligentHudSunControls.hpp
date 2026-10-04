#ifndef EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP
#define EVP_ARCHVIZ_DILIGENTHUDSUNCONTROLS_HPP

namespace geomsrv::archviz {
bool DrawSunStudyRange (const char* id, float& from, float& to, float minimum, float maximum, bool duration);
void DrawSunStudyGradient (bool blue, float& threshold);
} // namespace geomsrv::archviz
#endif
