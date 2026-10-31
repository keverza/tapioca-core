#include "Workspace.hpp"
#include <cstring>
#include <new>

namespace {
struct Session {
    TapiocaUiHost host;
    tapioca::playground::Workspace workspace;
};
} // namespace
uint32_t TapiocaUiAbiVersion (void)
{
    return TAPIOCA_UI_ABI_VERSION;
}
void* TapiocaUiCreate (const TapiocaUiHost* host, const char* initialJson)
{
    if (!host || host->size != sizeof (TapiocaUiHost) || !host->alloc || !host->release || !initialJson)
        return nullptr;
    try {
        auto parsed = tapioca::playground::json::Parse (initialJson);
        auto session = std::make_unique<Session> ();
        session->host = *host;
        if (!parsed.ok || !tapioca::playground::Restore (session->workspace, parsed.value))
            return nullptr;
        return session.release ();
    }
    catch (...) {
        return nullptr;
    }
}
void TapiocaUiDestroy (void* ui)
{
    try {
        delete static_cast<Session*> (ui);
    }
    catch (...) { /* Never unwind through the C ABI. */
    }
}
int TapiocaUiDraw (void* ui, const TapiocaUiFrame* frame, TapiocaUiResult* result)
{
    if (!ui || !frame || !result)
        return 0;
    *result = {};
    auto& session = *static_cast<Session*> (ui);
    try {
        const auto before = tapioca::playground::json::Write (session.workspace.document, 0);
        tapioca::playground::DrawWorkspace (session.workspace, *frame, *result);
        const auto after = tapioca::playground::json::Write (session.workspace.document, 0);
        if (after != before) {
            result->stateJson = static_cast<char*> (session.host.alloc (after.size () + 1));
            if (!result->stateJson)
                return 0;
            std::memcpy (result->stateJson, after.c_str (), after.size () + 1);
        }
        return 1;
    }
    catch (...) {
        return 0;
    }
}
