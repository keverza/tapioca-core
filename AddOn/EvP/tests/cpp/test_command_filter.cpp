// F2 — the palette search box's matcher and ranking.
//
// These tests assert ORDER, not numbers. The score constants are a ladder that
// will be tuned; what must not change silently is which command comes first for a
// given query, which is the whole reason this logic was pulled out of the shell.

#include "Palette/CommandFilter.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using evp::FuzzyScore;
using evp::ScoreCommand;
using evp::SearchableCommand;
using evp::SplitSearchTerms;

namespace {

SearchableCommand Cmd (const std::string& title,
                       const std::string& category = "General",
                       const std::string& folder   = "",
                       const std::vector<std::string>& tags = {})
{
    SearchableCommand c;
    c.title    = title;
    c.category = category;
    c.folder   = folder.empty () ? title : folder;
    c.tags     = tags;
    return c;
}

int Score (const SearchableCommand& c, const std::string& query)
{
    return ScoreCommand (c, SplitSearchTerms (query));
}

// The library the ranking tests share: a realistic slice of the command list.
const SearchableCommand kSlopeSymbols =
    Cmd ("Place Slope Symbols", "Annotation", "PlaceSlopeSymbols",
         { "roof", "slope", "pitch", "gradient" });
const SearchableCommand kRoofProbe =
    Cmd ("Roof Geometry Probe", "Diagnostics", "RoofProbe", { "roof", "mesh" });
const SearchableCommand kZoneReport =
    Cmd ("Zone Area Report", "Documentation", "ZoneReport", { "apartment", "area" });

}   // namespace

// ---------------------------------------------------------------------------
// Terms

TEST (SearchTerms, SplitsOnWhitespaceAndFoldsCase)
{
    const std::vector<std::string> terms = SplitSearchTerms ("  Roof   PROBE ");
    ASSERT_EQ (terms.size (), 2u);
    EXPECT_EQ (terms[0], "roof");
    EXPECT_EQ (terms[1], "probe");
}

TEST (SearchTerms, EmptyQueryMatchesEverything)
{
    EXPECT_TRUE (SplitSearchTerms ("").empty ());
    EXPECT_GT (Score (kZoneReport, ""), 0);
    EXPECT_GT (Score (kZoneReport, "   "), 0);
}

// ---------------------------------------------------------------------------
// Matching

TEST (CommandFilter, MatchesTitleCaseInsensitively)
{
    EXPECT_GT (Score (kSlopeSymbols, "slope"), 0);
    EXPECT_GT (Score (kSlopeSymbols, "SLOPE"), 0);
    EXPECT_GT (Score (kSlopeSymbols, "SyMbOlS"), 0);
}

TEST (CommandFilter, MatchesCategoryAndFolder)
{
    EXPECT_GT (Score (kSlopeSymbols, "annotation"), 0);
    EXPECT_GT (Score (kSlopeSymbols, "placeslopesymbols"), 0);
}

TEST (CommandFilter, MatchesATagThatAppearsInNoOtherField)
{
    // "pitch" is nowhere in the title, category or folder — this is what tags buy.
    EXPECT_EQ (kSlopeSymbols.title.find ("pitch"), std::string::npos);
    EXPECT_GT (Score (kSlopeSymbols, "pitch"), 0);
}

TEST (CommandFilter, TermsAreAndedNotOred)
{
    EXPECT_GT (Score (kRoofProbe, "roof probe"), 0);
    // "roof" hits, "apartment" does not -> the command is out, not half in.
    EXPECT_EQ (Score (kRoofProbe, "roof apartment"), 0);
}

TEST (CommandFilter, NoMatchScoresZero)
{
    EXPECT_EQ (Score (kZoneReport, "wallschedule"), 0);
}

// ---------------------------------------------------------------------------
// Ranking — the part a human cannot check by eye

TEST (CommandRanking, TitleBeatsTag)
{
    // Both match "slope": one by name, one only by association. The one the user
    // literally typed the name of must come first.
    const SearchableCommand tagged = Cmd ("Roof Analysis", "Diagnostics", "RoofAnalysis",
                                          { "slope" });
    EXPECT_GT (Score (kSlopeSymbols, "slope"), Score (tagged, "slope"));
}

TEST (CommandRanking, TagBeatsFuzzy)
{
    // A real tag hit must outrank letters that merely happen to appear in order.
    const SearchableCommand scattered = Cmd ("Section Line Optimiser Placement Editor");
    EXPECT_GT (Score (kRoofProbe, "roof"), Score (scattered, "roof"));
}

TEST (CommandRanking, ExactTitleBeatsPrefixBeatsSubstring)
{
    const SearchableCommand exact  = Cmd ("Roof");
    const SearchableCommand prefix = Cmd ("Roof Geometry Probe");
    const SearchableCommand inside = Cmd ("Rebuild Roof Cache");

    EXPECT_GT (Score (exact, "roof"),  Score (prefix, "roof"));
    EXPECT_GT (Score (prefix, "roof"), Score (inside, "roof"));
}

TEST (CommandRanking, MoreTermsMatchedRanksHigher)
{
    EXPECT_GT (Score (kRoofProbe, "roof probe"), Score (kRoofProbe, "roof"));
}

TEST (CommandRanking, CategoryQueryKeepsItsGroupTogether)
{
    // Typing a category is how the list is browsed, not searched: every command in
    // it must survive the filter.
    EXPECT_GT (Score (kRoofProbe, "diagnostics"), 0);
    EXPECT_EQ (Score (kSlopeSymbols, "diagnostics"), 0);
}

// ---------------------------------------------------------------------------
// Fuzzy

TEST (Fuzzy, MatchesAnInitialismAndRejectsAMissingLetter)
{
    EXPECT_GT (FuzzyScore ("Place Slope Symbols", "pss"), 0);
    EXPECT_EQ (FuzzyScore ("Place Slope Symbols", "psz"), 0);
}

TEST (Fuzzy, OutOfOrderIsNotAMatch)
{
    EXPECT_EQ (FuzzyScore ("Roof Probe", "eborf"), 0);
}

TEST (Fuzzy, ConsecutiveBeatsScattered)
{
    EXPECT_GT (FuzzyScore ("roof", "roof"), FuzzyScore ("remove old outer frame", "roof"));
}

TEST (Fuzzy, WordStartsBeatMidWord)
{
    // "ps" as two initials should outrank the same two letters buried in one word.
    EXPECT_GT (FuzzyScore ("Place Symbols", "ps"), FuzzyScore ("Collapse Stack", "ps"));
}

TEST (Fuzzy, ATypoStillFindsTheCommand)
{
    // The point of having fuzzy at all: "symbls" is a dropped letter, not a
    // different command.
    EXPECT_GT (Score (kSlopeSymbols, "symbls"), 0);
}

TEST (Fuzzy, TermLongerThanTheTextCannotMatch)
{
    EXPECT_EQ (FuzzyScore ("roof", "roofing surfaces"), 0);
}
