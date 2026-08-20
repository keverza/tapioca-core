#include "Palette/CommandFilter.hpp"

#include <algorithm>

namespace {

// ASCII fold only — see the header for why, and what tags are for.
char Fold (char c)
{
    return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

bool IsSpace (char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// What counts as the end of a word for the "word start" bonus. Includes the case
// boundary's stand-ins that command titles and folders actually use.
bool IsWordBreak (char c)
{
    return IsSpace (c) || c == '_' || c == '-' || c == '/' || c == '.' || c == '\\';
}

std::string FoldAll (const std::string& s)
{
    std::string out (s);
    for (char& c : out)
        c = Fold (c);
    return out;
}

// Index of `needle` in `haystack`, both already folded, or npos.
size_t Find (const std::string& haystack, const std::string& needle)
{
    return haystack.find (needle);
}

// The score ladder. Named rather than inlined so a test can state the ORDER it
// expects without hard-coding arithmetic, and so changing one rung is one edit.
constexpr int ExactTitle    = 1000;
constexpr int TitlePrefix   = 700;
constexpr int TitleWordHead = 600;
constexpr int ExactTag      = 500;
constexpr int TitleAnywhere = 400;
constexpr int TagPrefix     = 300;
constexpr int CategoryHit   = 260;
constexpr int TagAnywhere   = 240;
constexpr int FolderHit     = 200;

// Fuzzy always ranks below every literal hit above: it is the net that catches a
// typo or an abbreviation, never the reason a worse command outranks a better one.
constexpr int FuzzyTitleCap = 180;
constexpr int FuzzyOtherCap = 120;

}   // namespace

namespace evp {

std::vector<std::string> SplitSearchTerms (const std::string& query)
{
    std::vector<std::string> terms;
    std::string              current;

    for (const char c : query) {
        if (IsSpace (c)) {
            if (!current.empty ()) {
                terms.push_back (current);
                current.clear ();
            }
            continue;
        }
        current.push_back (Fold (c));
    }
    if (!current.empty ())
        terms.push_back (current);

    return terms;
}

int FuzzyScore (const std::string& text, const std::string& term)
{
    if (term.empty () || text.empty () || term.size () > text.size ())
        return 0;

    int    score = 0;
    int    run   = 0;      // how many characters have matched back-to-back
    size_t h     = 0;

    for (size_t n = 0; n < term.size (); ++n) {
        bool found = false;
        while (h < text.size ()) {
            const bool wordStart = (h == 0) || IsWordBreak (text[h - 1]);
            if (Fold (text[h]) == Fold (term[n])) {
                score += 1;
                score += 3 * run;             // consecutive characters are the signal
                if (wordStart)
                    score += 5;               // "psy" should find "Place Slope sYmbols"
                ++run;
                ++h;
                found = true;
                break;
            }
            run = 0;                          // a skipped character breaks the run
            ++h;
        }
        if (!found)
            return 0;                         // a character of the term is simply absent
    }

    // A tight match in a short name beats the same letters scattered through a long
    // one. +1 on the divisor so a single-character text cannot divide by zero.
    const int density = (int) ((100 * term.size ()) / (text.size () + 1));
    return score + density;
}

namespace {

// The best this one term can score against one field group.
int ScoreTerm (const evp::SearchableCommand& cmd,
               const std::string&            title,
               const std::string&            category,
               const std::string&            folder,
               const std::string&            term)
{
    int best = 0;

    if (title == term)
        return ExactTitle;                    // nothing can beat it; stop here

    const size_t inTitle = Find (title, term);
    if (inTitle == 0)
        best = std::max (best, TitlePrefix);
    else if (inTitle != std::string::npos)
        best = std::max (best, IsWordBreak (title[inTitle - 1]) ? TitleWordHead : TitleAnywhere);

    for (const std::string& rawTag : cmd.tags) {
        const std::string tag = FoldAll (rawTag);
        if (tag == term) {
            best = std::max (best, ExactTag);
            continue;
        }
        const size_t inTag = Find (tag, term);
        if (inTag == 0)
            best = std::max (best, TagPrefix);
        else if (inTag != std::string::npos)
            best = std::max (best, TagAnywhere);
    }

    if (Find (category, term) != std::string::npos)
        best = std::max (best, CategoryHit);
    if (Find (folder, term) != std::string::npos)
        best = std::max (best, FolderHit);

    if (best > 0)
        return best;

    // Nothing matched literally — fall back to fuzzy, capped below every rung above.
    best = std::max (best, std::min (FuzzyScore (title, term), FuzzyTitleCap));
    for (const std::string& rawTag : cmd.tags)
        best = std::max (best, std::min (FuzzyScore (FoldAll (rawTag), term), FuzzyOtherCap));
    best = std::max (best, std::min (FuzzyScore (folder, term), FuzzyOtherCap));
    best = std::max (best, std::min (FuzzyScore (category, term), FuzzyOtherCap));

    return best;
}

}   // namespace

int ScoreCommand (const SearchableCommand& command, const std::vector<std::string>& terms)
{
    if (terms.empty ())
        return 1;                             // no query: everything matches, equally

    const std::string title    = FoldAll (command.title);
    const std::string category = FoldAll (command.category);
    const std::string folder   = FoldAll (command.folder);

    int total = 0;
    for (const std::string& term : terms) {
        const int termScore = ScoreTerm (command, title, category, folder, term);
        if (termScore == 0)
            return 0;                         // AND: every term must find something
        total += termScore;
    }
    return total;
}

}   // namespace evp
