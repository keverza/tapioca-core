#include "Palette/CommandScan.hpp"

namespace {

std::string Utf8 (const GS::UniString& s)
{
    return std::string (s.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
}

} // namespace

namespace evp {

SearchableCommand Searchable (const CommandInfo& info)
{
    SearchableCommand out;
    out.title = Utf8 (info.title);
    out.category = Utf8 (info.category);
    out.folder = Utf8 (info.folder);
    for (const GS::UniString& tag : info.tags)
        out.tags.push_back (Utf8 (tag));
    return out;
}

std::vector<std::string> SearchTermsOf (const GS::UniString& query)
{
    return SplitSearchTerms (Utf8 (query));
}

} // namespace evp
