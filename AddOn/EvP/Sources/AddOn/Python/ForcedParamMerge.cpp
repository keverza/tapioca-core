#include "Python/ForcedParamMerge.hpp"

namespace evp {

std::string MergeForcedParams (const std::string& paramsJson, const std::string& overridesJson)
{
    if (overridesJson.empty () || overridesJson == "{}")
        return paramsJson;
    if (paramsJson.size () < 2 || paramsJson.front () != '{' || paramsJson.back () != '}' ||
        overridesJson.size () < 2 || overridesJson.front () != '{' || overridesJson.back () != '}')
        return paramsJson;

    std::string merged = paramsJson.substr (0, paramsJson.size () - 1);
    if (merged.size () > 1)
        merged += ',';
    merged += overridesJson.substr (1);
    return merged;
}

} // namespace evp
