#ifndef GEOMETRYSERVER_PALETTE_COMMANDSCAN_HPP
#define GEOMETRYSERVER_PALETTE_COMMANDSCAN_HPP

#include "Python/CommandCatalog.hpp"
#include "Palette/CommandFilter.hpp" // SearchableCommand — the search's view of a command

namespace evp {

// F2 — the command as the search box sees it, UniString folded to UTF-8. It lives
// here rather than in the shell because it is a VIEW OF CommandInfo; the matcher
// itself is deliberately DevKit-free and so cannot include this header.
SearchableCommand Searchable (const CommandInfo& info);

// The search box's text as terms. Same boundary, same reason: UniString becomes
// UTF-8 here, so the shell never has to know the matcher speaks std::string.
std::vector<std::string> SearchTermsOf (const GS::UniString& query);

} // namespace evp

#endif
