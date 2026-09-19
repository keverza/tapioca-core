// ArchViz/PatchProfile -- see the header. Every rule about what this refuses and
// why is there; this is the mechanism. MAIN THREAD ONLY.

#include "ArchViz/PatchProfile.hpp"

#include "ArchViz/ArchVizLog.hpp" // ArchVizLog
#include "Python/PathUtils.hpp"   // EvpDataDir, PathExists, ReadTextFile, WriteTextFile

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace geomsrv {
namespace archviz {
namespace patchprofile {

namespace {

Identity g_current;
bool g_computed = false;

// UTF-8 out of a UniString, the way PathUtils does it. The no-argument
// `ToCStr()` is a different (locale) conversion; do not substitute it.
std::string Utf8 (const GS::UniString& text)
{
    return std::string (text.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

std::string LowerAscii (std::string text)
{
    std::transform (text.begin (), text.end (), text.begin (), [] (unsigned char c) { return char (::tolower (c)); });
    return text;
}

std::string HexOf (const unsigned char* bytes, size_t count)
{
    static const char* kDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve (count * 2);
    for (size_t i = 0; i < count; ++i) {
        hex.push_back (kDigits[bytes[i] >> 4]);
        hex.push_back (kDigits[bytes[i] & 0x0f]);
    }
    return hex;
}

// ---- SHA-256 over a file ---------------------------------------------------
// ⚠️ STREAMED, NOT READ WHOLE. `Archicad.exe` is hundreds of megabytes; loading
// it into a buffer to hash it would spike this process's working set by that
// much, inside Archicad, for a diagnostic. A megabyte at a time costs the same
// disk and no memory worth naming.
//
// ⚠️ BCrypt, NOT A VENDORED SHA-256. The whole point of the profile is that its
// hashes can be reproduced by the user with `certutil -hashfile` or PowerShell's
// `Get-FileHash` on the same file -- a hash only this add-on can compute is a
// number nobody can check. Windows' own implementation is the one those tools
// use.
bool HashFile (const std::wstring& path, std::string& hashHex, uint64_t& sizeBytes, std::string& error)
{
    HANDLE file = ::CreateFileW (path.c_str (), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "could not open the file for hashing (GetLastError " + std::to_string (::GetLastError ()) + ")";
        return false;
    }

    LARGE_INTEGER size = {};
    if (::GetFileSizeEx (file, &size))
        sizeBytes = uint64_t (size.QuadPart);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> hashObject;
    bool ok = false;
    do {
        if (!BCRYPT_SUCCESS (::BCryptOpenAlgorithmProvider (&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            error = "BCryptOpenAlgorithmProvider(SHA256) failed";
            break;
        }
        DWORD objectSize = 0;
        DWORD written = 0;
        if (!BCRYPT_SUCCESS (::BCryptGetProperty (algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR) &objectSize,
                                                  sizeof (objectSize), &written, 0))) {
            error = "BCryptGetProperty(OBJECT_LENGTH) failed";
            break;
        }
        hashObject.resize (objectSize);
        if (!BCRYPT_SUCCESS (::BCryptCreateHash (algorithm, &hash, hashObject.data (), objectSize, nullptr, 0, 0))) {
            error = "BCryptCreateHash failed";
            break;
        }

        std::vector<unsigned char> buffer (1u << 20);
        for (;;) {
            DWORD read = 0;
            if (!::ReadFile (file, buffer.data (), DWORD (buffer.size ()), &read, nullptr)) {
                error = "ReadFile failed while hashing (GetLastError " + std::to_string (::GetLastError ()) + ")";
                break;
            }
            if (read == 0) {
                ok = true;
                break;
            }
            if (!BCRYPT_SUCCESS (::BCryptHashData (hash, buffer.data (), read, 0))) {
                error = "BCryptHashData failed";
                break;
            }
        }
        if (!ok)
            break;

        ok = false;
        unsigned char digest[32] = {};
        if (!BCRYPT_SUCCESS (::BCryptFinishHash (hash, digest, sizeof (digest), 0))) {
            error = "BCryptFinishHash failed";
            break;
        }
        hashHex = HexOf (digest, sizeof (digest));
        ok = true;
    } while (false);

    if (hash != nullptr)
        ::BCryptDestroyHash (hash);
    if (algorithm != nullptr)
        ::BCryptCloseAlgorithmProvider (algorithm, 0);
    ::CloseHandle (file);
    return ok;
}

// The file's own version resource, as `a.b.c.d`. "?" when it carries none --
// which is not a failure: the hash is what decides, and the version is the human
// half of the answer.
std::string FileVersionOf (const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW (path.c_str (), &ignored);
    if (size == 0)
        return "?";
    std::vector<unsigned char> block (size);
    if (!::GetFileVersionInfoW (path.c_str (), 0, size, block.data ()))
        return "?";
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (!::VerQueryValueW (block.data (), L"\\", (LPVOID*) &fixed, &fixedSize) || fixed == nullptr)
        return "?";
    char text[64] = {};
    std::snprintf (text, sizeof (text), "%u.%u.%u.%u", unsigned (HIWORD (fixed->dwFileVersionMS)),
                   unsigned (LOWORD (fixed->dwFileVersionMS)), unsigned (HIWORD (fixed->dwFileVersionLS)),
                   unsigned (LOWORD (fixed->dwFileVersionLS)));
    return std::string (text);
}

std::wstring ModulePath (HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW (module, path, MAX_PATH);
    return (length == 0) ? std::wstring () : std::wstring (path, length);
}

std::string NarrowPath (const std::wstring& wide)
{
    if (wide.empty ())
        return std::string ();
    const int needed =
        ::WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), int (wide.size ()), nullptr, 0, nullptr, nullptr);
    std::string narrow (size_t (needed), '\0');
    ::WideCharToMultiByte (CP_UTF8, 0, wide.c_str (), int (wide.size ()), narrow.data (), needed, nullptr, nullptr);
    return narrow;
}

// The leaf name, lower-cased, so a pin written on one install path verifies on
// another. The PATH is recorded too, but it is reported, never compared: a user
// who installed Archicad on D: is running the same bytes as one who installed it
// on C:, and the hash already says so.
std::string LeafNameLower (const std::wstring& path)
{
    const size_t slash = path.find_last_of (L"\\/");
    const std::wstring leaf = (slash == std::wstring::npos) ? path : path.substr (slash + 1);
    return LowerAscii (NarrowPath (leaf));
}

GS::UniString PinPath ()
{
    // ⚠️ NO SUBDIRECTORY, for the reason `ExperimentGuard` gives: this file sits
    // next to `EXPERIMENT_ARMED` and `SAFE_MODE` in the Tapioca root, because
    // every instruction that names it is one a user follows while something is
    // already wrong.
    const GS::UniString root = evp::EvpDataDir ();
    return root.IsEmpty () ? GS::UniString () : root + GS::UniString ("\\PATCH_PROFILE");
}

void ComputeCurrent ()
{
    if (g_computed)
        return;
    g_computed = true;

    // ⚠️ THE HOST IS THE RUNNING EXECUTABLE, NOT A NAME WE LOOK FOR.
    // `GetModuleFileNameW(nullptr)` answers with whatever actually loaded this
    // add-on -- which is `Archicad.exe` in every real session, and is something
    // else entirely under a test host or a Graphisoft internal build. Searching
    // for a file called Archicad.exe instead would happily hash a DIFFERENT
    // installation from the one running us.
    const std::wstring hostPath = ModulePath (nullptr);
    if (hostPath.empty ()) {
        g_current.why = "GetModuleFileNameW could not name the running executable";
        return;
    }
    g_current.hostPath = NarrowPath (hostPath);
    g_current.hostVersion = FileVersionOf (hostPath);

    std::string error;
    if (!HashFile (hostPath, g_current.hostSha256, g_current.hostSizeBytes, error)) {
        g_current.why = "the running executable could not be hashed: " + error;
        return;
    }
    g_current.valid = true;
    ArchVizLog ("patch profile: host " + g_current.hostVersion + " sha256 " + g_current.hostSha256.substr (0, 16) +
                "...");
}

const ModuleIdentity* FindModule (const std::vector<ModuleIdentity>& modules, const std::string& name)
{
    for (const ModuleIdentity& module : modules) {
        if (module.name == name)
            return &module;
    }
    return nullptr;
}

const TargetIdentity* FindTarget (const std::vector<TargetIdentity>& targets, const std::string& name)
{
    for (const TargetIdentity& target : targets) {
        if (target.name == name)
            return &target;
    }
    return nullptr;
}

// ---- the pin file ----------------------------------------------------------
// A flat, line-per-fact text file, deliberately. It is read by a user on a
// machine where something is already wrong, diffed against another machine's,
// and copied between them; JSON would buy nothing and cost readability.

bool LoadPin (Identity& pinned, std::string& error)
{
    const GS::UniString path = PinPath ();
    if (path.IsEmpty ()) {
        error = "%LOCALAPPDATA% is unavailable, so no patch profile can be read";
        return false;
    }
    if (!evp::PathExists (path)) {
        error = "no patch profile is pinned on this machine";
        return false;
    }
    GS::UniString contents;
    if (!evp::ReadTextFile (path, contents)) {
        error = "the patch profile at " + Utf8 (path) + " could not be read";
        return false;
    }

    std::istringstream stream (Utf8 (contents));
    std::string line;
    while (std::getline (stream, line)) {
        if (!line.empty () && line.back () == '\r')
            line.pop_back ();
        if (line.empty () || line[0] == '#')
            continue;
        std::istringstream fields (line);
        std::string kind;
        fields >> kind;
        if (kind == "host") {
            fields >> pinned.hostSha256 >> pinned.hostSizeBytes >> pinned.hostVersion;
            std::string rest;
            std::getline (fields, rest);
            if (!rest.empty () && rest[0] == ' ')
                rest.erase (0, 1);
            pinned.hostPath = rest;
            pinned.valid = true;
        }
        else if (kind == "module") {
            ModuleIdentity module;
            fields >> module.name >> module.sha256 >> module.sizeBytes >> module.version;
            pinned.modules.push_back (module);
        }
        else if (kind == "target") {
            TargetIdentity target;
            std::string rva;
            fields >> target.name >> target.module >> rva >> target.prologue;
            target.rva = std::strtoull (rva.c_str (), nullptr, 16);
            pinned.targets.push_back (target);
        }
    }

    if (!pinned.valid) {
        error = "the patch profile at " + Utf8 (path) +
                " has no host line; it is not a "
                "profile this build wrote. Delete it and pin again";
        return false;
    }
    return true;
}

} // namespace

const Identity& Current ()
{
    ComputeCurrent ();
    return g_current;
}

void RecordModule (const wchar_t* moduleName)
{
    ComputeCurrent ();
    HMODULE module = ::GetModuleHandleW (moduleName);
    if (module == nullptr)
        return; // not loaded; nothing patches it, so nothing to pin
    const std::wstring path = ModulePath (module);
    if (path.empty ())
        return;
    const std::string name = LeafNameLower (path);
    if (FindModule (g_current.modules, name) != nullptr)
        return;

    ModuleIdentity identity;
    identity.name = name;
    identity.version = FileVersionOf (path);
    std::string error;
    if (!HashFile (path, identity.sha256, identity.sizeBytes, error)) {
        // Recorded with an empty hash rather than dropped. ⚠️ A MODULE THAT
        // CANNOT BE HASHED MUST STILL REACH `Verify`, where an empty hash can
        // never match a pinned one and the install refuses. Dropping it here
        // would make an unreadable system DLL look like a module nobody patches.
        ArchVizLog ("patch profile: " + name + " could not be hashed (" + error + "); it will not verify");
    }
    g_current.modules.push_back (identity);
}

void RecordTarget (const char* name, const void* fn)
{
    ComputeCurrent ();
    if (fn == nullptr || name == nullptr)
        return;

    TargetIdentity target;
    target.name = name;

    HMODULE owner = nullptr;
    if (::GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (fn), &owner) != 0 &&
        owner != nullptr) {
        target.module = LeafNameLower (ModulePath (owner));
        target.rva =
            uint64_t (reinterpret_cast<const unsigned char*> (fn) - reinterpret_cast<const unsigned char*> (owner));
    }
    else {
        // ⚠️ NO OWNING MODULE IS ITSELF A FINDING, not a reason to skip the
        // target. A vtable slot pointing at memory that belongs to no loaded
        // module is either a thunk somebody else installed or a wrong index --
        // both of which must refuse, and both of which are invisible if the
        // target is silently left out of the profile.
        target.module = "(none)";
    }

    target.prologue = HexOf (reinterpret_cast<const unsigned char*> (fn), kPrologueBytes);

    for (TargetIdentity& existing : g_current.targets) {
        if (existing.name == target.name) {
            existing = target;
            return;
        }
    }
    g_current.targets.push_back (target);
}

void ClearTargets ()
{
    ComputeCurrent ();
    g_current.targets.clear ();
}

bool HasPin ()
{
    const GS::UniString path = PinPath ();
    return !path.IsEmpty () && evp::PathExists (path);
}

std::string PinFilePath ()
{
    return Utf8 (PinPath ());
}

bool Verify (std::string& error)
{
    return VerifyDetailed (error) == Verdict::Ok;
}

Verdict VerifyDetailed (std::string& error)
{
    const Identity& current = Current ();
    if (!current.valid) {
        error = "this Archicad's identity could not be read (" + current.why +
                "), so the GPU-state hooks cannot be pinned to it and will not install";
        return Verdict::Refuse;
    }

    Identity pinned;
    if (!LoadPin (pinned, error)) {
        // ⚠️ THE REFUSAL SAYS HOW TO FIX IT. This is the message a user meets
        // first, on the machine where the mode simply did not arm, and "no
        // profile is pinned" without the next step is a dead end.
        error += ". Run Tapioca.ViewerPatchProfile {pin: true} on the exact Archicad "
                 "build you have tested, which writes " +
                 PinFilePath () +
                 "; until then the GPU-state hooks stay off and camera sync falls back "
                 "to the portable path";
        return Verdict::Refuse;
    }

    if (pinned.hostSha256 != current.hostSha256) {
        error = "this Archicad is not the pinned build -- pinned " + pinned.hostVersion + " (sha256 " +
                pinned.hostSha256.substr (0, 16) + "...), running " + current.hostVersion + " (sha256 " +
                current.hostSha256.substr (0, 16) +
                "...). The GPU-state hooks read Archicad's own GPU buffers and their "
                "layout is build-specific, so they will not install. Camera sync falls "
                "back to the portable path, which still works";
        return Verdict::Refuse;
    }

    // ⚠️ NO TARGETS RECORDED IS A REFUSAL, NOT A PASS, and the first live run
    // showed why it has to be said out loud: a `ViewerPatchProfile` query that
    // had not fingerprinted anything reported "VERIFIES" on the strength of the
    // host hash alone, because the loop below had nothing to iterate. That is a
    // green light for a check that did not happen. The executable being the same
    // says nothing about whether slot 44 still holds `RSSetViewports`.
    if (current.targets.empty ()) {
        error = "no hook targets have been fingerprinted in this session, so verifying now "
                "would check the executable's hash and nothing about the vtable slots being "
                "patched. Arm 'hookdiag' with gpuState so Archicad's own context can be "
                "found, then ask again";
        return Verdict::Refuse;
    }

    for (const TargetIdentity& target : current.targets) {
        const TargetIdentity* pin = FindTarget (pinned.targets, target.name);
        if (pin == nullptr) {
            error = target.name + " is about to be patched but is not in the pinned "
                                  "profile; re-pin on this machine";
            return Verdict::Refuse;
        }
        if (pin->module != target.module || pin->rva != target.rva) {
            error = target.name + " no longer sits where the profile pinned it (" + pin->module + "+0x" +
                    std::to_string (pin->rva) + " then, " + target.module + "+0x" + std::to_string (target.rva) +
                    " now); refusing to patch a slot whose meaning may have changed";
            return Verdict::Refuse;
        }
        if (pin->prologue != target.prologue) {
            // ⚠️ THIS IS THE CHECK THAT CATCHES SOMEBODY ELSE'S INLINE DETOUR.
            // A hot-patch trampoline rewrites exactly these bytes, and two
            // products hooking one function is precisely the race the vtable
            // technique was chosen to avoid. Refusing is the whole point.
            //
            // ⚠️ AND THE OTHER PRODUCT IS NAMED FIRST NOW,
            // BECAUSE IT IS THE COMMON CASE. Intel GPA and RenderDoc both do
            // exactly this, and a user who has just taken a capture reads "the
            // module was rebuilt" and goes looking for a Windows update that did
            // not happen. A rebuilt module moves the RVA as well; a detour
            // usually does not, which is why this branch is reached.
            error = target.name + "'s first bytes differ from the pinned profile (" + pin->prologue + " then, " +
                    target.prologue +
                    " now). Another product has almost certainly patched this "
                    "function already -- a graphics capture tool such as Intel GPA "
                    "or RenderDoc does exactly this. Close it and restart Archicad. "
                    "If none is running, the module was rebuilt and a re-pin is the fix";
            return Verdict::Refuse;
        }
    }

    // ---- and only NOW the modules, and only those that host a target --------
    //
    // ⚠️ A MODULE THAT HOSTS NO TARGET IS NOT CONSULTED.
    // `dxgi.dll` is recorded in the profile and hosts zero of the twenty-seven
    // slots; letting its hash veto the install is how a redistributable repair
    // killed the overlay for a day while every function being patched sat
    // untouched at the same offset with the same bytes.
    bool staleHostingModule = false;
    std::string staleDetail;
    for (const ModuleIdentity& module : current.modules) {
        bool hostsTarget = false;
        for (const TargetIdentity& target : current.targets) {
            if (target.module == module.name) {
                hostsTarget = true;
                break;
            }
        }
        if (!hostsTarget)
            continue;

        const ModuleIdentity* pin = FindModule (pinned.modules, module.name);
        if (pin == nullptr) {
            error = module.name + " hosts hook targets but is not in the pinned "
                                  "profile; re-pin on this machine";
            return Verdict::Refuse;
        }
        if (module.sha256.empty () || pin->sha256 != module.sha256) {
            staleHostingModule = true;
            staleDetail +=
                (staleDetail.empty () ? "" : ", ") + module.name + " " + pin->version + " -> " + module.version;
        }
    }

    if (staleHostingModule) {
        // ⚠️ NOT A REFUSAL. Every target in this module was
        // just checked at its recorded offset against its recorded first bytes
        // and matched. The vtable is intact; the module is merely a different
        // build of the same functions. Report it so the caller can re-pin in the
        // open rather than refuse in the dark.
        error = staleDetail + " changed since the profile was pinned, but all " +
                std::to_string (current.targets.size ()) +
                " hook targets still sit at their pinned offsets with their pinned "
                "first bytes, so the vtable is intact";
        return Verdict::StaleModuleTargetsIntact;
    }

    return Verdict::Ok;
}

bool Pin (std::string& error)
{
    const Identity& current = Current ();
    if (!current.valid) {
        error = "this Archicad's identity could not be read (" + current.why + "), so there is nothing to pin";
        return false;
    }
    if (current.targets.empty ()) {
        // ⚠️ A PROFILE WITH NO TARGETS WOULD VERIFY AND CHECK NOTHING. The host
        // hash alone says the executable is the same; the targets are what say
        // the slots we patch still hold what we think they hold. Refusing here
        // is what stops a pin taken before discovery from becoming a profile
        // that always passes.
        error = "no hook targets have been fingerprinted yet, so a profile written now "
                "would verify the executable and check nothing about the slots being "
                "patched; the caller must run vtable discovery first";
        return false;
    }

    const GS::UniString path = PinPath ();
    if (path.IsEmpty ()) {
        error = "%LOCALAPPDATA% is unavailable, so the patch profile cannot be written";
        return false;
    }

    std::ostringstream out;
    out << "# Tapioca patch profile -- the exact Archicad build the GPU-state camera\n"
           "# hooks are pinned to. Written by Tapioca.ViewerPatchProfile {pin: true}.\n"
           "#\n"
           "# The hooks refuse to install unless every line below still matches what is\n"
           "# running. Delete this file to turn the GPU-state path off entirely; camera\n"
           "# sync then falls back to the portable path, which still works.\n"
           "#\n"
           "# Hashes are SHA-256 and can be reproduced with:\n"
           "#   Get-FileHash -Algorithm SHA256 <file>\n"
           "version 1\n";
    out << "host " << current.hostSha256 << ' ' << current.hostSizeBytes << ' ' << current.hostVersion << ' '
        << current.hostPath << '\n';
    for (const ModuleIdentity& module : current.modules) {
        out << "module " << module.name << ' ' << module.sha256 << ' ' << module.sizeBytes << ' ' << module.version
            << '\n';
    }
    for (const TargetIdentity& target : current.targets) {
        char rva[32] = {};
        std::snprintf (rva, sizeof (rva), "%llx", (unsigned long long) target.rva);
        out << "target " << target.name << ' ' << target.module << ' ' << rva << ' ' << target.prologue << '\n';
    }

    GS::UniString writeError;
    if (!evp::WriteTextFile (path, out.str ().c_str (), writeError)) {
        error = "the patch profile could not be written to " + Utf8 (path) + " (" + Utf8 (writeError) + ")";
        return false;
    }
    ArchVizLog ("patch profile: pinned " + current.hostVersion + " with " + std::to_string (current.targets.size ()) +
                " targets to " + Utf8 (path));
    return true;
}

std::string PinnedSummary ()
{
    Identity pinned;
    std::string error;
    if (!LoadPin (pinned, error))
        return std::string ();
    return pinned.hostVersion + " (sha256 " + pinned.hostSha256.substr (0, 16) + "..., " +
           std::to_string (pinned.modules.size ()) + " modules, " + std::to_string (pinned.targets.size ()) +
           " targets)";
}

std::string CurrentSummary ()
{
    const Identity& current = Current ();
    if (!current.valid)
        return "unknown (" + current.why + ")";
    return current.hostVersion + " (sha256 " + current.hostSha256.substr (0, 16) + "..., " +
           std::to_string (current.modules.size ()) + " modules, " + std::to_string (current.targets.size ()) +
           " targets)";
}

} // namespace patchprofile
} // namespace archviz
} // namespace geomsrv
