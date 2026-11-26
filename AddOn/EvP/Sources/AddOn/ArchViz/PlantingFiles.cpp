#include "ArchViz/PlantingFiles.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

namespace geomsrv::archviz::planting {

namespace fs = std::filesystem;

std::string PathText (const fs::path& path)
{
    const auto text = path.u8string ();
    return std::string (text.begin (), text.end ());
}

fs::path TapiocaDataFolder (const wchar_t* leaf)
{
#ifdef _WIN32
    wchar_t* root = nullptr;
    size_t length = 0;
    if (_wdupenv_s (&root, &length, L"LOCALAPPDATA") != 0 || !root)
        return {};
    fs::path folder = fs::path (root) / L"Tapioca" / leaf;
    std::free (root);
    return folder;
#else
    const char* root = std::getenv ("LOCALAPPDATA");
    return root ? fs::path (root) / "Tapioca" / leaf : fs::path ();
#endif
}

namespace {

bool ReadText (const fs::path& path, std::string& text, std::string& error)
{
    std::ifstream file (path, std::ios::binary);
    if (!file) {
        error = "cannot open " + PathText (path.filename ());
        return false;
    }
    text.assign (std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> ());
    // A UTF-8 byte-order mark is allowed; the JSON reader does not expect one.
    if (text.size () >= 3 && text.compare (0, 3, "\xEF\xBB\xBF") == 0)
        text.erase (0, 3);
    return true;
}

} // namespace

bool WriteFileAtomically (const fs::path& path, const std::string& text, std::string& error)
{
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file (temp, std::ios::binary | std::ios::trunc);
        if (!file || !file.write (text.data (), std::streamsize (text.size ())) || !file.flush ()) {
            error = "cannot write " + PathText (temp.filename ());
            return false;
        }
    }
    std::error_code ec;
    fs::rename (temp, path, ec);
    if (ec) {
        fs::remove (temp, ec);
        error = "cannot replace " + PathText (path.filename ());
        return false;
    }
    return true;
}

bool SeedCatalogueFolder (const fs::path& folder, std::vector<std::string>& created, std::string& error)
{
    created.clear ();
    if (folder.empty ()) {
        error = "LOCALAPPDATA is unavailable";
        return false;
    }
    std::error_code ec;
    fs::create_directories (folder, ec);
    if (ec) {
        error = "cannot create " + PathText (folder) + ": " + ec.message ();
        return false;
    }
    const std::pair<const char*, const std::string*> seeds[] = { { kPlantsFile, &SeedPlantsJson () },
                                                                 { kProfilesFile, &SeedProfilesJson () } };
    for (const auto& [name, text] : seeds) {
        const fs::path path = folder / name;
        if (fs::exists (path, ec))
            continue;
        if (!WriteFileAtomically (path, *text, error))
            return false;
        created.push_back (name);
    }
    return true;
}

void ReadCatalogueFolder (const fs::path& folder, CatalogueState& state)
{
    state.folder = PathText (folder);
    state.problems.clear ();
    std::string text, error;
    ParseProblems problems;
    std::string plantsNote, profilesNote;
    if (!ReadText (folder / kPlantsFile, text, error)) {
        state.problems.push_back (std::string (kPlantsFile) + ": " + error);
    }
    else {
        Catalogue catalogue;
        if (ParseCatalogue (text, catalogue, problems)) {
            state.plants = std::move (catalogue);
            state.plantsLoaded = true;
            state.selected = -1;
        }
        else {
            for (const auto& item : problems.items)
                state.problems.push_back (std::string (kPlantsFile) + ": " + item);
        }
    }
    if (!ReadText (folder / kProfilesFile, text, error)) {
        state.problems.push_back (std::string (kProfilesFile) + ": " + error);
    }
    else {
        std::vector<Profile> profiles;
        if (ParseProfiles (text, profiles, problems)) {
            state.profiles = std::move (profiles);
            state.profilesLoaded = true;
        }
        else {
            for (const auto& item : problems.items)
                state.problems.push_back (std::string (kProfilesFile) + ": " + item);
        }
    }
    const size_t plants = state.plants.plants.size (), profiles = state.profiles.size ();
    state.status = "Read " + std::to_string (plants) + (plants == 1 ? " plant" : " plants") + " and " +
                   std::to_string (profiles) + (profiles == 1 ? " profile." : " profiles.");
    if (!state.problems.empty ())
        state.status = std::to_string (state.problems.size ()) +
                       (state.problems.size () == 1 ? " problem" : " problems") +
                       "; the previous catalogue stays in use. " + state.status;
}

bool RestoreCatalogueDefaults (const fs::path& folder, std::string& backup, std::string& error)
{
    std::vector<std::string> created;
    if (!SeedCatalogueFolder (folder, created, error))
        return false;
    const std::time_t now = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
    std::tm local {};
#ifdef _WIN32
    localtime_s (&local, &now);
#else
    localtime_r (&now, &local);
#endif
    char stamp[32] = {};
    std::strftime (stamp, sizeof (stamp), ".backup-%Y%m%d-%H%M%S.json", &local);
    backup = stamp;
    const std::pair<const char*, const std::string*> seeds[] = { { kPlantsFile, &SeedPlantsJson () },
                                                                 { kProfilesFile, &SeedProfilesJson () } };
    for (const auto& [name, text] : seeds) {
        const fs::path path = folder / name;
        if (std::find (created.begin (), created.end (), name) != created.end ())
            continue; // just seeded: nothing of the user's to keep
        fs::path copy = path;
        copy.replace_extension ();
        copy += stamp;
        std::error_code ec;
        fs::copy_file (path, copy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot back up " + std::string (name) + ": " + ec.message ();
            return false;
        }
        if (!WriteFileAtomically (path, *text, error))
            return false;
    }
    return true;
}

std::vector<std::string> ListWeatherFiles (const fs::path& folder, std::string& error)
{
    std::vector<std::string> names;
    if (folder.empty ()) {
        error = "LOCALAPPDATA is unavailable";
        return names;
    }
    std::error_code ec;
    fs::create_directories (folder, ec);
    for (fs::directory_iterator it (folder, ec), end; !ec && it != end; it.increment (ec)) {
        if (!it->is_regular_file (ec))
            continue;
        std::string ext = PathText (it->path ().extension ());
        std::transform (ext.begin (), ext.end (), ext.begin (),
                        [] (unsigned char c) { return char (std::tolower (c)); });
        if (ext == ".epw")
            names.push_back (PathText (it->path ().filename ()));
    }
    if (ec)
        error = "cannot list " + PathText (folder) + ": " + ec.message ();
    std::sort (names.begin (), names.end ());
    return names;
}

} // namespace geomsrv::archviz::planting
