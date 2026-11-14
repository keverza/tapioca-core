#ifndef EVP_SURFACETAKEOFFCOMMANDS_HPP
#define EVP_SURFACETAKEOFFCOMMANDS_HPP
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "Geometry/SurfaceTakeoffResult.hpp"
#include <functional>
namespace geomsrv {
const char* SurfaceTakeoffInputSchema ();
surfacetakeoff::Request SurfaceTakeoffRequestFromNative (const GS::ObjectState& params);
surfacetakeoff::Calculation CalculateExteriorSurfaceResult (const surfacetakeoff::Request& request,
                                                            const std::function<bool ()>& cancelled = {});
surfacetakeoff::Calculation CalculateExteriorSurfaceResult (const GS::ObjectState& params,
                                                            GS::ProcessControl& processControl,
                                                            const std::function<bool ()>& cancelled = {});
NativeCommandResult ToNativeSurfaceTakeoffResult (const surfacetakeoff::Result& result);
NativeCommandResult CalculateExteriorSurfaceTakeoff (const GS::ObjectState& params, GS::ProcessControl& processControl,
                                                     const std::function<bool ()>& cancelled = {});
NativeCommandRegistrations GetSurfaceTakeoffCommandRegistrations ();
} // namespace geomsrv
#endif
