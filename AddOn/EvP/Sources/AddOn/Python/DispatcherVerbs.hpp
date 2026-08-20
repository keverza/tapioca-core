#ifndef EVP_PYTHON_DISPATCHERVERBS_HPP
#define EVP_PYTHON_DISPATCHERVERBS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <cstddef>

namespace evp {

enum class DispatcherExecutionKind {
    Inline,
    PostMainThread,
    InvokeMainThread,
    TransactionReplay
};

struct DispatcherVerbRegistration {
    const char*                 name;
    DispatcherExecutionKind     execution;
    bool                        cancelExempt;
};

struct DispatcherVerbRegistrations {
    const DispatcherVerbRegistration* data;
    std::size_t                       size;

    const DispatcherVerbRegistration* begin () const { return data; }
    const DispatcherVerbRegistration* end () const { return data + size; }
};

DispatcherVerbRegistrations GetDispatcherVerbRegistrations ();
const DispatcherVerbRegistration* FindDispatcherVerb (const GS::UniString& name);

} // namespace evp

#endif
