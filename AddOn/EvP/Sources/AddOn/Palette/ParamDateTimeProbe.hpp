#ifndef EVP_PALETTE_PARAMDATETIMEPROBE_HPP
#define EVP_PALETTE_PARAMDATETIMEPROBE_HPP

#include "DGDateTime.hpp"
#include "UniString.hpp"

namespace evp {

void LogDateTimeProbe (const GS::UniString& name, const char* stage, DG::DateTime& control);

} // namespace evp

#endif
