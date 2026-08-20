#ifndef GEOMETRYSERVER_PALETTE_COMMANDFILTER_HPP
#define GEOMETRYSERVER_PALETTE_COMMANDFILTER_HPP

// F2 — what the palette's search box matches, and how well.
//
// DELIBERATELY FREE OF THE DEVKIT: std::string in, int out, no DG, no ACAPI, no
// GS::UniString. That is what lets it be covered by the offline C++ test suite
// (tests/cpp/test_command_filter.cpp), which is the only way to know a ranking
// change did what it meant to — the shell can only be judged by eye, in Archicad.
// The shell converts its UniStrings to UTF-8 at the boundary and calls in.
//
// Strings are UTF-8 and folded ASCII-only: "Roof" == "roof", but "Ä" != "ä". A
// command whose title needs case-insensitive diacritics should carry the folded
// spelling as a tag — which is exactly what tags are for.

#include <string>
#include <vector>

namespace evp {

// One command reduced to the fields the search looks at. Description is NOT among
// them on purpose: a word matching inside a sentence is noise, and it made every
// query return most of the list.
struct SearchableCommand {
    std::string              title;
    std::string              category;
    std::string              folder;
    std::vector<std::string> tags;   // @evp.command(tags=[...]) — never displayed
};

// Whitespace-split and ASCII-lowercased. An empty query yields no terms, which
// every command matches — the unfiltered list.
std::vector<std::string> SplitSearchTerms (const std::string& query);

// 0 == no match, higher == better. Terms are ANDed: every term must find SOMETHING
// (so "roof probe" narrows rather than widens), and a term's score is its best
// field. The ladder, highest first:
//
//   exact title  >  title prefix  >  title word start  >  exact tag  >  title
//   substring  >  category/tag/folder  >  fuzzy (subsequence) anywhere
//
// so a tag can surface a related command without ever burying the one actually
// named by what was typed.
int ScoreCommand (const SearchableCommand& command, const std::vector<std::string>& terms);

// Subsequence match with the usual bonuses: every character of `term` appears in
// `text` in order, scoring consecutive runs and word-start hits above scattered
// ones ("plsl" -> "PlaceSlopeSymbols"). 0 when a character is missing. Exposed so
// the ranking can be tested one rule at a time, and so this is the one function to
// replace if a real matcher (fts_fuzzy_match, rapidfuzz) is ever vendored.
int FuzzyScore (const std::string& text, const std::string& term);

}   // namespace evp

#endif
