#include "NodeGraph/FaultBarrier.hpp"

#include <cstdio>
#include <exception>

#if defined(_MSC_VER)
// excpt.h, not windows.h: __try/__except and EXCEPTION_EXECUTE_HANDLER come from
// the CRT, so the barrier compiles into the offline test binary and the graph
// core stays free of Win32, DG and the DevKit.
#include <excpt.h>
#endif

namespace evp::nodegraph {
namespace {

#if defined(_MSC_VER)

// A C++ exception reaches the SEH layer as this code. It must pass THROUGH, or
// the __except below would swallow it and the caller's catch clauses would never
// run - turning every ordinary node error into an unhelpful "structured fault".
constexpr unsigned long kCppExceptionCode = 0xE06D7363UL;

const char* FaultName (unsigned long code)
{
    switch (code) {
        case 0xC0000005UL:
            return "access violation";
        case 0xC0000006UL:
            return "in-page error";
        case 0xC000001DUL:
            return "illegal instruction";
        case 0xC0000025UL:
            return "noncontinuable exception";
        case 0xC000008EUL:
            return "floating-point division by zero";
        case 0xC0000094UL:
            return "integer division by zero";
        case 0xC0000096UL:
            return "privileged instruction";
        case 0xC00000FDUL:
            return "stack overflow";
        default:
            return "structured exception";
    }
}

int FaultFilter (unsigned long code, unsigned long* captured)
{
    if (code == kCppExceptionCode)
        return EXCEPTION_CONTINUE_SEARCH;
    *captured = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Deliberately holds no object that needs unwinding, which is what lets __try
// coexist with C++ exception handling in this translation unit.
bool InvokeGuarded (const std::function<bool ()>& body, bool* result, unsigned long* code)
{
    __try {
        *result = body ();
        return true;
    }
    __except (FaultFilter (GetExceptionCode (), code)) {
        return false;
    }
}

#endif

} // namespace

GuardOutcome RunGuarded (const std::function<bool ()>& body)
{
    GuardOutcome outcome;
    if (!body) {
        outcome.fault = "no node implementation";
        return outcome;
    }

    try {
#if defined(_MSC_VER)
        bool result = false;
        unsigned long code = 0;
        if (InvokeGuarded (body, &result, &code)) {
            outcome.completed = true;
            outcome.result = result;
            return outcome;
        }
        char text[128];
        std::snprintf (text, sizeof text, "%s (0x%08lX)", FaultName (code), code);
        outcome.fault = text;
        return outcome;
#else
        outcome.completed = true;
        outcome.result = body ();
        return outcome;
#endif
    }
    catch (const std::exception& exception) {
        outcome.completed = false;
        outcome.fault = exception.what ();
    }
    catch (...) {
        outcome.completed = false;
        outcome.fault = "unknown exception";
    }
    return outcome;
}

} // namespace evp::nodegraph
