#ifndef EVP_PYTHON_WEBUIRUNSTATE_HPP
#define EVP_PYTHON_WEBUIRUNSTATE_HPP

#include "Array.hpp"
#include "UniString.hpp"

#include <cstdint>
#include <mutex>

namespace evp {

struct WebUIRunSnapshot {
    bool active = false;
    uint64_t generation = 0;
    GS::UniString title;
    GS::UniString status;
    GS::Array<GS::UniString> headers;
    GS::Array<GS::UniString> rows;
    GS::UniString resultText;
};

// The browser is a second presentation of the one command runtime. This state
// carries only the run's presentation data across the worker, HTTP, and browser
// threads; command execution remains in CommandLaunch/CommandRunner.
class WebUIRunState {
  public:
    static WebUIRunState& Get ();

    bool Begin (uint64_t generation, const GS::UniString& title);
    void SetStatus (const GS::UniString& status);
    void SetResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rows);
    void SetResultText (const GS::UniString& text);
    void Finish (uint64_t generation, const GS::UniString& status);

    WebUIRunSnapshot Snapshot () const;

  private:
    WebUIRunState () = default;

    mutable std::mutex mutex;
    WebUIRunSnapshot state;
};

} // namespace evp

#endif
