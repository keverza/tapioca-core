#include "DispatcherVerbs.hpp"

namespace evp {

namespace {

constexpr DispatcherVerbRegistration verbs[] = {
    { "GetCommands", DispatcherExecutionKind::InvokeMainThread, true },
    { "RunCommand", DispatcherExecutionKind::InvokeMainThread, true },
    { "GetRunState", DispatcherExecutionKind::Inline, true },
    { "CancelRun", DispatcherExecutionKind::Inline, true },
    { "GetServerState", DispatcherExecutionKind::Inline, true },
    { "StartServer", DispatcherExecutionKind::InvokeMainThread, true },
    { "StopServer", DispatcherExecutionKind::InvokeMainThread, true },
    { "SetTracing", DispatcherExecutionKind::Inline, true },
    { "GetErrorTrail", DispatcherExecutionKind::Inline, true },
    { "SetStatus", DispatcherExecutionKind::PostMainThread, true },
    { "ShowAlert", DispatcherExecutionKind::PostMainThread, true },
    { "ShowResults", DispatcherExecutionKind::PostMainThread, false },
    { "ShowResultText", DispatcherExecutionKind::PostMainThread, false },
    { "GetCurrentParams", DispatcherExecutionKind::InvokeMainThread, false },
    { "PollCancel", DispatcherExecutionKind::Inline, true },
    { "PollSelectionPrompt", DispatcherExecutionKind::Inline, true },
    { "ShowSelectionPrompt", DispatcherExecutionKind::PostMainThread, false },
    { "HideSelectionPrompt", DispatcherExecutionKind::PostMainThread, true },
    { "CommitTransaction", DispatcherExecutionKind::TransactionReplay, false }
};

} // namespace

DispatcherVerbRegistrations GetDispatcherVerbRegistrations ()
{
    return { verbs, sizeof (verbs) / sizeof (verbs[0]) };
}

const DispatcherVerbRegistration* FindDispatcherVerb (const GS::UniString& name)
{
    for (const DispatcherVerbRegistration& verb : verbs) {
        if (name == verb.name)
            return &verb;
    }
    return nullptr;
}

} // namespace evp
