#ifndef EVP_PLANOVERLAY_OVERLAYOWNERSHIP_HPP
#define EVP_PLANOVERLAY_OVERLAYOWNERSHIP_HPP

namespace geomsrv {
namespace planoverlay {

enum class Owner { None, NativeCommand, Watch };

struct SessionOwnership {
    Owner owner = Owner::None;
    bool ownsWindow = false;
    bool startedTracking = false;
};

enum class ReleaseAction { None, StopTracking, DestroyWindow };

ReleaseAction DetermineReleaseAction (const SessionOwnership& session, Owner currentOwner, bool sameWindow);

} // namespace planoverlay
} // namespace geomsrv

#endif
