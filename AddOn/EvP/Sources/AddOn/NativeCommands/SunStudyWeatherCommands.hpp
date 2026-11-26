#ifndef EVP_NATIVECOMMANDS_SUNSTUDYWEATHERCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYWEATHERCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// Tapioca.CheckWeatherSun -- #155 P2's live acceptance: Archicad's sun for
// every record of a weather file, on the project's standard-time clock with
// daylight saving off, against the independent sun. Reports the verdict and
// the raw place values the time base rests on; changes nothing in the model.
//
// A DOMAIN OF ITS OWN: the sun-study verbs' file is near its size cap, and
// check_cpp.py's REGISTRY rule asks one provider per NativeCommands/*Commands.cpp.
NativeCommandRegistrations GetSunStudyWeatherCommandRegistrations ();

} // namespace geomsrv

#endif
