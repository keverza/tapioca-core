#ifndef EVP_ARCHVIZ_PLANTINGFILES_HPP
#define EVP_ARCHVIZ_PLANTINGFILES_HPP

// ArchViz/PlantingFiles -- the Planting tab's local files (#278 Stage 3,
// #155 P1): %LOCALAPPDATA%\Tapioca\Planting (plants.json, profiles.json) and
// %LOCALAPPDATA%\Tapioca\Weather (*.epw).
//
// ⚠️ NEVER OVERWRITE USER DATA. Seeding writes only missing files; Restore
// defaults copies both files to a dated backup first. Writes go to a temporary
// file and are renamed into place, so a crash leaves the old file or the new
// one, never half of either. Nothing is downloaded.

#include "ArchViz/PlantingHudModel.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace geomsrv::archviz::planting {

// %LOCALAPPDATA%\Tapioca\<leaf>; empty when LOCALAPPDATA is unset.
std::filesystem::path TapiocaDataFolder (const wchar_t* leaf);

constexpr const char* kPlantsFile = "plants.json";
constexpr const char* kProfilesFile = "profiles.json";

// Create `folder` and write each missing seed file. `created` names what was
// written.
bool SeedCatalogueFolder (const std::filesystem::path& folder, std::vector<std::string>& created, std::string& error);

// Read both files. A file that fails keeps the previous plants or profiles in
// `state` and adds its problems; `state.status` says what happened.
void ReadCatalogueFolder (const std::filesystem::path& folder, CatalogueState& state);

// Copy both files to "<name>.backup-YYYYMMDD-HHMMSS.json", then write the
// seeds. `backup` names the backup suffix.
bool RestoreCatalogueDefaults (const std::filesystem::path& folder, std::string& backup, std::string& error);

constexpr const char* kCsvImportFile = "plants.csv";
constexpr const char* kCsvExportFile = "plants-export.csv";

// Write the catalogue to <folder>/plants-export.csv (replacing an earlier export).
bool ExportCatalogueCsv (const std::filesystem::path& folder, const Catalogue& catalogue, std::string& error);

// Read <folder>/plants.csv; when it converts and validates, copy plants.json
// to a dated backup, write the CSV's plants as plants.json, and read the
// folder again. Any problem changes nothing and is listed in `state`.
bool ImportCatalogueCsv (const std::filesystem::path& folder, CatalogueState& state, std::string& backup);

// Write `text` to `path` through a temporary file and a rename.
bool WriteFileAtomically (const std::filesystem::path& path, const std::string& text, std::string& error);

// The .epw files directly in `folder`, by name; the folder is created.
std::vector<std::string> ListWeatherFiles (const std::filesystem::path& folder, std::string& error);

// UTF-8 of a path.
std::string PathText (const std::filesystem::path& path);

} // namespace geomsrv::archviz::planting

#endif
