#ifndef EVP_VERSIONREQUIREMENTS_HPP
#define EVP_VERSIONREQUIREMENTS_HPP

#include "UniString.hpp"

namespace evp {

// A declaration is an AND-list of comparisons, e.g. ">=1.5.2 <2.0". The
// scanner validates this grammar before it reaches C++; false therefore means
// either an unmet floor or a malformed manually-constructed CommandInfo.
bool SatisfiesVersionRequirement (const GS::UniString& installed,
                                  const GS::UniString& requirement);

// Resolves Tapir once, on the first command that needs it. A failed lookup is
// deliberately cached as 0.0.0: absence must refuse dependent commands without
// turning startup/rescan into an add-on availability check.
GS::UniString InstalledTapirVersion ();

GS::UniString VersionRequirementFailure (const GS::UniString& product,
                                         const GS::UniString& installed,
                                         const GS::UniString& requirement);

} // namespace evp

#endif
