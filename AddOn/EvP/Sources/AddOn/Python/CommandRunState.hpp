#ifndef EVP_PYTHON_COMMANDRUNSTATE_HPP
#define EVP_PYTHON_COMMANDRUNSTATE_HPP

#include "Array.hpp"
#include "UniString.hpp"

#include <cstdint>
#include <mutex>

namespace evp {

struct CommandRunSnapshot {
    bool active = false;
    uint64_t generation = 0;
    GS::UniString title;
    GS::UniString status;
    GS::Array<GS::UniString> headers;
    GS::Array<GS::UniString> rows;
    GS::UniString resultText;
};

// Presentation data for bus-launched commands, shared across worker and HTTP
// threads. Command execution remains in CommandLaunch/CommandRunner.
class CommandRunState {
  public:
    static CommandRunState& Get ();

    bool Begin (uint64_t generation, const GS::UniString& title);
    void SetStatus (const GS::UniString& status);
    void SetResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rows);
    void SetResultText (const GS::UniString& text);
    void Finish (uint64_t generation, const GS::UniString& status);

    CommandRunSnapshot Snapshot () const;

  private:
    CommandRunState () = default;

    mutable std::mutex mutex;
    CommandRunSnapshot state;
};

} // namespace evp

#endif
