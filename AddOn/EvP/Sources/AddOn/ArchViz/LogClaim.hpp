#ifndef EVP_ARCHVIZ_LOGCLAIM_HPP
#define EVP_ARCHVIZ_LOGCLAIM_HPP

// ArchViz/LogClaim -- which file a process's log writes when another process already
// writes it. ArchVizLog holds the one it takes for the session; this only chooses it.
//
// ⚠️ ONE WRITER A FILE, AND A SECOND ONE IS NEVER SILENT. Every Archicad that loads the
// add-on logs under the same %LOCALAPPDATA%\Tapioca -- two Archicad 29s, or 27 beside
// 29. The log used to be held sharing only reads, so the second process's open was
// refused and every line of its session went nowhere, without a word: a run read from
// archviz.log then looked like a build that did nothing. Now the second takes the next
// free "<name>-2<ext>", "<name>-3<ext>" ..., and says so in BOTH places: a line in each
// file it could not take, naming the one it went to -- archviz.log is where everyone
// looks -- and one at the head of its own.
//
// ⚠️ WHO WRITES A FILE IS ASKED OF WINDOWS, NOT RECORDED. A writer holds its file
// sharing reads and writes: reads so it can be tailed, writes so another process can
// leave that pointer line. A claim asks first with an open that shares only reads,
// which Windows refuses while any process holds the file for writing. A process that
// dies lets go of its handle, so no claim outlives its writer. Two processes asking in
// the same instant can both pass the question; they then share one file and lose
// nothing.
//
// Plain Win32 and the standard library, so the offline suite takes it with two writers.

#include <cstdint>
#include <string>

namespace geomsrv {
namespace archviz {
namespace logclaim {

// How many processes' files there are room for: `base`, then "-2" to "-9".
inline constexpr unsigned kMaxWriters = 9;

// The file the n-th writer of `base` takes: `base` itself for 1, "<name>-<n><ext>" after.
std::wstring WriterPath (const std::wstring& base, unsigned n);

// What rotation renames `path` to: "<name>.1<ext>", the scheme every host log shares
// (Python/PathUtils.cpp).
std::wstring BackupPath (const std::wstring& path);

struct Claim {
    void* file = nullptr; // a HANDLE opened for appending; nullptr when no file could be taken
    std::wstring path;
    unsigned writer = 0; // 1 for `base` itself
    uint64_t bytes = 0;  // the file's size once taken, its head line included
};

// The first of `base`'s files no other process writes, opened for appending; the lines
// left on the way begin with `stamp` (ArchVizLog's clock). The folder is the caller's to
// create. The caller closes `file`.
Claim Take (const std::wstring& base, const std::string& stamp);

} // namespace logclaim
} // namespace archviz
} // namespace geomsrv

#endif
