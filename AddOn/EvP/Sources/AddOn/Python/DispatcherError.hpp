#ifndef EVP_DISPATCHERERROR_HPP
#define EVP_DISPATCHERERROR_HPP

#include "ObjectState.hpp"
#include "UniString.hpp"

namespace evp {

inline GS::ObjectState MakeError (const char* code, const GS::UniString& message, const GS::UniString& detail)
{
    GS::ObjectState error;
    error.Add ("code", GS::UniString (code));
    error.Add ("message", message);
    // ObjectState cannot hold JSON null; an absent detail key has the same wire
    // meaning for callers using error.get("detail").
    if (!detail.IsEmpty ())
        error.Add ("detail", detail);
    return error;
}

} // namespace evp

#endif
