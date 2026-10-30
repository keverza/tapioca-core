#include "Python/CommandRunState.hpp"

#include "Python/RunCancel.hpp"

namespace evp {

CommandRunState& CommandRunState::Get ()
{
    static CommandRunState state;
    return state;
}

bool CommandRunState::Begin (uint64_t generation, const GS::UniString& title)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state.active)
        return false;

    state.active = true;
    state.generation = generation;
    state.title = title;
    state.status = "Queued";
    state.headers.Clear ();
    state.rows.Clear ();
    state.resultText.Clear ();
    return true;
}

void CommandRunState::SetStatus (const GS::UniString& status)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state.active)
        state.status = status;
}

void CommandRunState::SetResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rows)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!state.active)
        return;
    state.headers = headers;
    state.rows = rows;
    state.resultText.Clear ();
}

void CommandRunState::SetResultText (const GS::UniString& text)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state.active) {
        state.resultText = text;
        state.headers.Clear ();
        state.rows.Clear ();
    }
}

void CommandRunState::Finish (uint64_t generation, const GS::UniString& status)
{
    RunCancel::Get ().EndRun (generation);

    std::lock_guard<std::mutex> lock (mutex);
    if (!state.active || state.generation != generation)
        return;
    state.active = false;
    state.status = status;
}

CommandRunSnapshot CommandRunState::Snapshot () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return state;
}

} // namespace evp
