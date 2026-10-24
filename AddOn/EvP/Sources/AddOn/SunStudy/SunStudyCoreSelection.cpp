#include "SunStudy/SunStudyCoreSelection.hpp"

#include <atomic>
#include <cstdlib>
#include <string>

namespace evp::sunstudy {

namespace {

SunStudyCoreKind FromEnvironment ()
{
    const char* value = std::getenv ("TAPIOCA_SUNSTUDY_CORE");
    return value != nullptr && std::string (value) == "rust" ? SunStudyCoreKind::Rust : SunStudyCoreKind::Cpp;
}

// -1: no override; otherwise the SunStudyCoreKind value.
std::atomic<int> g_override { -1 };

} // namespace

bool RustSunStudyCoreAvailable ()
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    return true;
#else
    return false;
#endif
}

SunStudyCoreKind SelectedSunStudyCore ()
{
    if (!RustSunStudyCoreAvailable ())
        return SunStudyCoreKind::Cpp;
    const int forced = g_override.load ();
    if (forced >= 0)
        return static_cast<SunStudyCoreKind> (forced);
    static const SunStudyCoreKind fromEnvironment = FromEnvironment ();
    return fromEnvironment;
}

void OverrideSunStudyCore (std::optional<SunStudyCoreKind> kind)
{
    g_override.store (kind.has_value () ? static_cast<int> (*kind) : -1);
}

} // namespace evp::sunstudy
