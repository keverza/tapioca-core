#pragma once

#include <string>
#include <vector>

namespace geomsrv::archviz::test {
// Capture only the calling test thread; normal offline logging remains a no-op.
class LogCapture {
  public:
    LogCapture ();
    ~LogCapture ();
    LogCapture (const LogCapture&) = delete;
    LogCapture& operator= (const LogCapture&) = delete;
    std::vector<std::string> lines;

  private:
    LogCapture* previous = nullptr;
};
} // namespace geomsrv::archviz::test
