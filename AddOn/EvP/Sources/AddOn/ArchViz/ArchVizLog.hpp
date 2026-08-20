#ifndef EVP_ARCHVIZ_ARCHVIZLOG_HPP
#define EVP_ARCHVIZ_ARCHVIZLOG_HPP

// ArchViz/ArchVizLog — one line into %LOCALAPPDATA%\Tapioca\logs\archviz.log.
//
// ⚠️ THIS USED TO LIVE IN BgfxCallback.hpp, and it was the only thing in that
// header the rest of the viewer needed. bgfx's trace callback and the viewer's
// own lifecycle lines went into ONE file on purpose -- "init said X, then we did
// Y" is only readable when both halves are interleaved in order -- so the log
// function was declared beside the callback that fed it. When bgfx was removed
// (PLAT-RE66) fourteen files still included that header for this one function.
//
// The file name is unchanged: `archviz.log` is what every probe's NOW LOOK block
// and every handoff tells the user to read, and renaming it would strand all of
// them for no gain.
//
// ⚠️ CALLABLE FROM ANY THREAD, AND THAT IS LOAD-BEARING. The render thread, the
// extraction thread and the main thread all write here. It takes a mutex, and it
// reaches `evp::AppendTextLine` (Python/PathUtils), which is ACAPI-FREE by
// design -- that is what makes it legal off the main thread.
//
// It rotates at the shared 5 MiB cap, like every other host log (CLAUDE.md,
// "Where output goes"). Nothing here needs a flag; it is always on.

#include <string>

namespace geomsrv {
namespace archviz {

void ArchVizLog (const std::string& line);

}   // namespace archviz
}   // namespace geomsrv

#endif
