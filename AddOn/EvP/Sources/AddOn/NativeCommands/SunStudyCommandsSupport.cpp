#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/SunStudyCommandsSupport.hpp"

#include "NativeCommands/CommandUtils.hpp" // Base64Encode / Base64Decode
#include "SunStudy/SunStudyStore.hpp"

#include <cstring>

namespace geomsrv {
namespace sunstudysupport {

std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

GS::UniString Text (const std::string& text)
{
    return GS::UniString (text.c_str (), CC_UTF8);
}

std::string ReadStudyId (const GS::ObjectState& params)
{
    GS::UniString id;
    if (params.Get ("studyId", id) && !id.IsEmpty ())
        return Utf8 (id);

    const std::vector<std::string> ids = evp::sunstudy::SunStudyStore::Get ().Ids ();
    return ids.empty () ? std::string () : ids.back ();
}

GS::Int32 ReadInt (const GS::ObjectState& params, const char* key, GS::Int32 fallback)
{
    GS::Int32 value = 0;
    return params.Get (key, value) ? value : fallback;
}

double ReadDouble (const GS::ObjectState& params, const char* key, double fallback)
{
    double value = 0.0;
    return params.Get (key, value) ? value : fallback;
}

std::string ReadString (const GS::ObjectState& params, const char* key, const char* fallback)
{
    GS::UniString value;
    if (params.Get (key, value) && !value.IsEmpty ())
        return Utf8 (value);
    return std::string (fallback);
}

std::vector<std::string> ReadStringList (const GS::ObjectState& params, const char* key)
{
    std::vector<std::string> out;
    GS::Array<GS::UniString> values;
    if (!params.Get (key, values))
        return out;
    out.reserve (values.GetSize ());
    for (UInt32 i = 0; i < values.GetSize (); ++i)
        if (!values[i].IsEmpty ())
            out.push_back (Utf8 (values[i]));
    return out;
}

GS::UniString PackDoubles (const std::vector<double>& values)
{
    std::vector<unsigned char> bytes (values.size () * sizeof (double));
    if (!values.empty ())
        std::memcpy (bytes.data (), values.data (), bytes.size ());
    return Base64Encode (bytes);
}

bool UnpackDoubles (const GS::UniString& text, std::vector<double>& values)
{
    std::vector<unsigned char> bytes;
    if (!Base64Decode (text, bytes))
        return false;
    if (bytes.size () % sizeof (double) != 0)
        return false;
    values.resize (bytes.size () / sizeof (double));
    if (!values.empty ())
        std::memcpy (values.data (), bytes.data (), bytes.size ());
    return true;
}

GS::UniString PackBits (const std::vector<uint8_t>& flags)
{
    std::vector<unsigned char> bytes ((flags.size () + 7) / 8, 0);
    for (size_t i = 0; i < flags.size (); ++i) {
        if (flags[i] != 0)
            bytes[i / 8] |= (unsigned char) (1u << (i % 8));
    }
    return Base64Encode (bytes);
}

} // namespace sunstudysupport
} // namespace geomsrv
