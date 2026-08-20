#ifndef EVP_NATIVECOMMANDS_NATIVECOMMANDRESULT_HPP
#define EVP_NATIVECOMMANDS_NATIVECOMMANDRESULT_HPP

#include "ObjectState.hpp"

#include <utility>

namespace geomsrv {

enum class NativeCommandFailureKind {
    Command,
    SchemaValidation
};

struct NativeCommandResult {
    bool                     ok = false;
    GS::ObjectState          data;
    GS::UniString            error;
    NativeCommandFailureKind failureKind = NativeCommandFailureKind::Command;

    NativeCommandResult () = default;
    NativeCommandResult (const GS::ObjectState& value) : ok (true), data (value) {}
    NativeCommandResult (GS::ObjectState&& value) : ok (true), data (std::move (value)) {}

    static NativeCommandResult Failure (
        const GS::UniString& message,
        NativeCommandFailureKind kind = NativeCommandFailureKind::Command)
    {
        NativeCommandResult result;
        result.error = message;
        result.failureKind = kind;
        return result;
    }
};

} // namespace geomsrv

#endif
