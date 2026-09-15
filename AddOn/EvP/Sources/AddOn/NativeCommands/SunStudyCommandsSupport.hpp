#ifndef EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDSSUPPORT_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDSSUPPORT_HPP

// NativeCommands/SunStudyCommandsSupport — the parameter reading and bulk
// packing shared by the sun study's two command domains.
//
// ⚠️ IT IS NOT A COMMAND PROVIDER AND MUST NOT BECOME ONE. `SunStudyCommands`
// (the analysis verbs) and `SunStudyDisplayCommands` (the overlay verbs) are
// separate REGISTRY domains precisely because one file exports exactly one
// provider. This file exports none: it exists so that splitting them did not
// mean maintaining two copies of the same base64 packer, which is how two
// callers of "the same" wire format start disagreeing about it.
//
// ⚠️ NO ANALYSIS AND NO DISPLAY LIVES HERE. Everything below is about turning an
// ObjectState into C++ values and back. The moment something in here knows what
// a sun study IS, it belongs in `SunStudy/` instead.

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ObjectState.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace sunstudysupport {

std::string Utf8 (const GS::UniString& text);
GS::UniString Text (const std::string& text);

// The study a verb operates on when it names none. Keeps a console session short
// while the contract stays multi-study underneath.
std::string ReadStudyId (const GS::ObjectState& params);

GS::Int32 ReadInt (const GS::ObjectState& params, const char* key, GS::Int32 fallback);
double ReadDouble (const GS::ObjectState& params, const char* key, double fallback);
std::string ReadString (const GS::ObjectState& params, const char* key, const char* fallback);

// ⚠️ BULK ARRAYS TRAVEL PACKED, AND THIS IS NOT A MICRO-OPTIMISATION. A live
// study of 176,106 samples over 49 timesteps measured 1,209 ms of ANALYSIS
// inside 15,636 ms of call: fourteen of those seconds were the wire, carrying a
// million doubles up as JSON text and 8.6 million step bits back the same way.
// Packed, the same payloads are base64 over raw bytes -- one bit per step
// instead of two characters, eight bytes per coordinate instead of twenty --
// and they parse in one pass instead of eight million allocations.
//
// The plain arrays stay for small studies and for anything reading by eye; a
// caller asks for packed when the size is worth it.
GS::UniString PackDoubles (const std::vector<double>& values);
bool UnpackDoubles (const GS::UniString& text, std::vector<double>& values);

// One BIT per (sample, step), sample-major, LSB first within each byte. A step
// bit is one of two values, so a byte per step wastes seven eighths of the wire.
GS::UniString PackBits (const std::vector<uint8_t>& flags);

} // namespace sunstudysupport
} // namespace geomsrv

#endif // EVP_NATIVECOMMANDS_SUNSTUDYCOMMANDSSUPPORT_HPP
