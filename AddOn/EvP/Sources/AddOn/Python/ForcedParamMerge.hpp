#ifndef EVP_PYTHON_FORCEDPARAMMERGE_HPP
#define EVP_PYTHON_FORCEDPARAMMERGE_HPP

#include <string>

namespace evp {

// Both inputs are scanner/framework-owned JSON objects. Forced members are placed
// last, so Python's JSON decoder gives them authority over duplicate UI keys.
std::string MergeForcedParams (const std::string& paramsJson, const std::string& overridesJson);

} // namespace evp

#endif
