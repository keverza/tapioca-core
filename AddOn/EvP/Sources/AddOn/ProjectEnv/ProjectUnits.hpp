#ifndef EVP_PROJECTENV_PROJECTUNITS_HPP
#define EVP_PROJECTENV_PROJECTUNITS_HPP

// The project's working units (Options > Project Preferences > Working Units) as the editor's
// display units (Edit/EditDisplayUnits.hpp, #109). Main thread only: it reads Archicad's
// preferences and publishes them to the snapshot every thread reads. Imperial units are not
// converted yet: such a project is shown in metres and `imperial` is set.
#include "Edit/EditDisplayUnits.hpp"

namespace geomsrv::projectenv {

// Reads the preferences and publishes them; false (the snapshot reset to metres) when they cannot
// be read, as with no project open.
bool RefreshProjectUnits ();
// Back to metres, for a closed project: units never outlive their project.
void ForgetProjectUnits ();

} // namespace geomsrv::projectenv
#endif
