#include "Python/WebUIRunState.hpp"

#include "Python/RunCancel.hpp"

namespace evp {

WebUIRunState& WebUIRunState::Get ()
{
    static WebUIRunState state;
    return state;
}

bool WebUIRunState::Begin (uint64_t generation, const GS::UniString& title)
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

void WebUIRunState::SetStatus (const GS::UniString& status)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state.active)
        state.status = status;
}

void WebUIRunState::SetResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rows)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (!state.active)
        return;
    state.headers = headers;
    state.rows = rows;
    state.resultText.Clear ();
}

void WebUIRunState::SetResultText (const GS::UniString& text)
{
    std::lock_guard<std::mutex> lock (mutex);
    if (state.active) {
        state.resultText = text;
        state.headers.Clear ();
        state.rows.Clear ();
    }
}

void WebUIRunState::Finish (uint64_t generation, const GS::UniString& status)
{
    RunCancel::Get ().EndRun (generation);

    std::lock_guard<std::mutex> lock (mutex);
    if (!state.active || state.generation != generation)
        return;
    state.active = false;
    state.status = status;
}

WebUIRunSnapshot WebUIRunState::Snapshot () const
{
    std::lock_guard<std::mutex> lock (mutex);
    return state;
}

} // namespace evp
