#ifndef EVP_GH2_OVERLAY_REQUEST_HPP
#define EVP_GH2_OVERLAY_REQUEST_HPP

// BOUND BY OVERLAY-INVARIANTS.md: this admits content, never a camera or renderer.
// The native command's strict schema remains the authority for geometry/style.
#include <cstdint>
#include <string>
#include <string_view>

namespace evp::grasshopper {

inline std::string Gh2OverlayPrefix (uint32_t generation)
{
    return "gh2." + std::to_string (generation) + ".";
}

inline bool IsGh2OverlayLayer (std::string_view name, uint32_t generation)
{
    const std::string prefix = Gh2OverlayPrefix (generation);
    if (generation == 0 || name.rfind (prefix, 0) != 0 || name.size () != prefix.size () + 32)
        return false;
    for (const char c : name.substr (prefix.size ()))
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
            return false;
    return true;
}

inline bool IsGh2OverlayRequest (std::string_view command, std::string_view parameters, uint32_t generation)
{
    if (command == "Tapioca.Overlay2D" || command == "Tapioca.Overlay3D")
        return parameters == "{\"action\":\"on\"}";
    if (command != "Tapioca.SetOverlayLayer" && command != "Tapioca.ClearOverlayLayer")
        return false;
    constexpr std::string_view start = "{\"layer\":\"";
    if (parameters.size () > 1024 * 1024 || parameters.rfind (start, 0) != 0)
        return false;
    const size_t end = parameters.find ('"', start.size ());
    if (end == std::string_view::npos ||
        !IsGh2OverlayLayer (parameters.substr (start.size (), end - start.size ()), generation))
        return false;
    const std::string_view tail = parameters.substr (end);
    if (command == "Tapioca.ClearOverlayLayer")
        return tail == "\"}"; // never all:true or somebody else's layer
    return (tail.rfind ("\",\"views\":\"2d\",", 0) == 0 || tail.rfind ("\",\"views\":\"3d\",", 0) == 0) &&
           parameters.back () == '}';
}

} // namespace evp::grasshopper
#endif
