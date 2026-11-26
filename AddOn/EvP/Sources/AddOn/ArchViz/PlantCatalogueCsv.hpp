#ifndef EVP_ARCHVIZ_PLANTCATALOGUECSV_HPP
#define EVP_ARCHVIZ_PLANTCATALOGUECSV_HPP

// ArchViz/PlantCatalogueCsv -- the plant catalogue as a spreadsheet (#278
// Stage 3): CSV export and import, and the catalogue written back as JSON.
//
// ⚠️ JSON STAYS THE SOURCE OF TRUTH. An imported CSV is converted to JSON,
// validated by ParseCatalogue exactly as a hand-edited plants.json would be,
// and only then replaces plants.json (after a backup). A CSV with any problem
// changes nothing.
//
// One row per plant. Lists (light classes, flowering months) are joined with
// '|'. Columns are matched by header name, in any order; an unknown column is
// a problem (a misspelt header would otherwise drop its data silently). UTF-8,
// comma-separated, RFC 4180 quoting. Problems found after conversion name the
// plant as plants[i], the i-th plant row (0-based).

#include "ArchViz/PlantCatalogue.hpp"

#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

// The columns export writes and import understands, in export order.
const std::vector<std::string>& CatalogueCsvColumns ();

// One row per plant, with a header row. UTF-8 with a byte-order mark so a
// spreadsheet opens Lithuanian names correctly.
std::string CatalogueToCsv (const Catalogue& catalogue);

// Parse CSV text into rows of fields (RFC 4180: quotes, doubled quotes,
// commas and line breaks inside quotes). False with a line number on an
// unterminated quote.
bool ParseCsv (const std::string& text, std::vector<std::vector<std::string>>& rows, std::string& error);

// Build a catalogue from CSV rows: `base` gives the id, locale and source
// titles kept from the current catalogue; a source named only in the CSV gets
// its id as title. Problems name the CSV line and column.
bool CatalogueFromCsv (const std::string& text, const Catalogue& base, Catalogue& out, ParseProblems& problems);

// The catalogue as plants.json text (schema v1), readable by ParseCatalogue.
std::string CatalogueToJson (const Catalogue& catalogue);

} // namespace geomsrv::archviz::planting

#endif
