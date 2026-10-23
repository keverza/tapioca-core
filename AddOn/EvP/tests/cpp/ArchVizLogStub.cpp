// ArchViz/ArchVizLog for the offline suite. The add-on writes its log through Archicad's
// own file API into its data folder; here a line goes nowhere, so a source that narrates
// what it did (OverlayInput.cpp) can be tested for what it does.

#include "ArchViz/ArchVizLog.hpp"
#include "archviz_log_fixture.hpp"
#include <utility>

namespace geomsrv {
namespace archviz {
namespace {
thread_local test::LogCapture* s_capture = nullptr;
}

test::LogCapture::LogCapture () : previous (std::exchange (s_capture, this))
{
}

test::LogCapture::~LogCapture ()
{
    s_capture = previous;
}

void ArchVizLog (const std::string& line)
{
    if (s_capture)
        s_capture->lines.push_back (line);
}

void ArchVizLogClose ()
{
}

} // namespace archviz
} // namespace geomsrv
