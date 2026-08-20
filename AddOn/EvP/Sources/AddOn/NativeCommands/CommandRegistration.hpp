#ifndef EVP_NATIVECOMMANDS_COMMANDREGISTRATION_HPP
#define EVP_NATIVECOMMANDS_COMMANDREGISTRATION_HPP

#include "NativeCommands/CommandBase.hpp"

#include <cstddef>
#include <memory>

namespace geomsrv {

struct NativeCommandRegistration;
using NativeCommandMaker = std::unique_ptr<MainThreadCommand> (*) (const NativeCommandRegistration&);

struct NativeCommandRegistration {
    const char*        name;
    NativeCommandMaker make;
    bool               cancelExempt = false;
    const char*        inputSchema = nullptr;
    const char*        responseSchema = nullptr;
};

struct NativeCommandRegistrations {
    const NativeCommandRegistration* data;
    std::size_t                      size;

    const NativeCommandRegistration* begin () const { return data; }
    const NativeCommandRegistration* end () const { return data + size; }
};

template<class Command>
class RegisteredNativeCommand final : public Command {
public:
    explicit RegisteredNativeCommand (const NativeCommandRegistration& registration) : registration (registration) {}

    GS::String GetName () const override { return registration.name; }

    GS::Optional<GS::UniString> GetInputParametersSchema () const override
    {
        if (registration.inputSchema != nullptr)
            return GS::UniString (registration.inputSchema);
        return Command::GetInputParametersSchema ();
    }

    GS::Optional<GS::UniString> GetResponseSchema () const override
    {
        if (registration.responseSchema != nullptr)
            return GS::UniString (registration.responseSchema);
        return Command::GetResponseSchema ();
    }

private:
    const NativeCommandRegistration& registration;
};

template<class Command>
std::unique_ptr<MainThreadCommand> MakeRegisteredNativeCommand (const NativeCommandRegistration& registration)
{
    return std::make_unique<RegisteredNativeCommand<Command>> (registration);
}

template<std::size_t Size>
NativeCommandRegistrations MakeRegistrationView (const NativeCommandRegistration (&items)[Size])
{
    return { items, Size };
}

} // namespace geomsrv

#endif
