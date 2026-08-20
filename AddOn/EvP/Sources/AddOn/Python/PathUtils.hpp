#ifndef EVP_PATHUTILS_HPP
#define EVP_PATHUTILS_HPP

// Filesystem/env helpers shared by PythonHost and the P1 gate spike.
//
// Deliberately ACAPI-FREE: only GS::UniString + Win32. That is what makes this
// the one layer of the .apx that can be compiled with the real /Zc:wchar_t- and
// exercised offline against the real GSRoot (GSRoot links standalone and needs
// no Archicad init). Keep it that way — if ACAPI creeps in, the tests die.

#include "UniString.hpp"

namespace evp {

bool ReadEnv (const wchar_t* name, GS::UniString& value);
bool PathExists (const GS::UniString& path);

// Creates `dir` and any missing parents.
bool CreateDirectoryChain (const GS::UniString& dir);

bool WriteTextFile (const GS::UniString& path, const char* utf8, GS::UniString& error);
bool WriteTextFile (const GS::UniString& path, const GS::UniString& text, GS::UniString& error);

// Reads a whole UTF-8 file. False if it does not exist or cannot be read.
bool ReadTextFile (const GS::UniString& path, GS::UniString& text);

// Appends one line, opening and closing per call so the file is complete on
// disk even if the process dies mid-run — the point of the gate-spike log,
// which has to survive a deadlock and a force-quit.
bool AppendTextLine (const GS::UniString& path, const GS::UniString& line);

// %LOCALAPPDATA%\Tapioca (empty if %LOCALAPPDATA% is unset). Migrates the
// legacy EvP root once when the Tapioca root does not yet exist.
GS::UniString EvpDataDir ();

// %LOCALAPPDATA%\Tapioca\logs\scan.log — where the palette reports what the command
// scanner found and what it refused. Here rather than hand-rolled at each call
// site: the palette shell and the parameter panel both write to it.
GS::UniString ScanLogPath ();

// One breadcrumb into logs\startup.log, flushed before it returns.
//
// ⚠️ THIS IS FOR CODE THAT MIGHT NOT SURVIVE THE NEXT STATEMENT. A hard crash
// (as opposed to a returned error) leaves nothing at all behind, so the ONLY
// evidence of how far a startup path got is what it wrote before dying — and
// that only works if each line is on disk before the next statement runs, which
// AppendTextLine guarantees by opening and closing per call.
//
// Two Archicad sessions were lost inside ControlPalette's constructor with no
// trace beyond "palette.json was written and scan.log was not", which localised
// the death to three statements and no further. These breadcrumbs are what
// close that gap. Cheap enough to leave in: a handful of lines per startup.
void StartupTrace (const GS::UniString& message);

} // namespace evp

#endif
