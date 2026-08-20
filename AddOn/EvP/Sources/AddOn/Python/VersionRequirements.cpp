#include "Python/VersionRequirements.hpp"

#include "Python/ApiDispatcher.hpp"

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Version { int major = 0; int minor = 0; int patch = 0; };

std::string Utf8 (const GS::UniString& value)
{
    return std::string (value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

bool ParseVersion (const std::string& text, Version& out)
{
    std::istringstream input (text);
    std::string part;
    int* fields[] = { &out.major, &out.minor, &out.patch };
    int index = 0;
    while (std::getline (input, part, '.')) {
        if (index == 3 || part.empty ()) return false;
        for (unsigned char c : part) if (!std::isdigit (c)) return false;
        *fields[index++] = std::stoi (part);
    }
    return index > 0;
}

int Compare (const Version& a, const Version& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool MatchesClause (const Version& installed, const std::string& clause)
{
    const size_t first = clause.find_first_of ("0123456789");
    if (first == std::string::npos) return false;
    Version wanted;
    if (!ParseVersion (clause.substr (first), wanted)) return false;
    const int compare = Compare (installed, wanted);
    const std::string op = clause.substr (0, first);
    return op == ">=" ? compare >= 0 : op == ">" ? compare > 0 :
           op == "<=" ? compare <= 0 : op == "<" ? compare < 0 :
           (op == "=" || op == "==") ? compare == 0 : false;
}

} // namespace

namespace evp {

bool SatisfiesVersionRequirement (const GS::UniString& installed, const GS::UniString& requirement)
{
    if (requirement.IsEmpty ()) return true;
    Version actual;
    if (!ParseVersion (Utf8 (installed), actual)) return false;
    std::istringstream clauses (Utf8 (requirement));
    std::string clause;
    while (clauses >> clause)
        if (!MatchesClause (actual, clause)) return false;
    return true;
}

GS::UniString InstalledTapirVersion ()
{
    static std::once_flag once;
    static GS::UniString version ("0.0.0");
    std::call_once (once, [] () {
        const GS::UniString response = DispatchApiCall ("Tapir.GetAddOnVersion", "{}");
        GS::ObjectState envelope, data;
        bool ok = false;
        if (JSON::ConvertToObjectState (response, envelope) == NoError &&
            envelope.Get ("ok", ok) && ok && envelope.Get ("data", data)) {
            GS::UniString reported;
            if (data.Get ("version", reported) && !reported.IsEmpty ())
                version = reported;
        }
    });
    return version;
}

GS::UniString VersionRequirementFailure (const GS::UniString& product,
                                         const GS::UniString& installed,
                                         const GS::UniString& requirement)
{
    return GS::UniString::Printf ("This command needs %T %T; installed is %T.",
                                  product.ToPrintf (), requirement.ToPrintf (), installed.ToPrintf ());
}

} // namespace evp
