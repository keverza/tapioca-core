#ifndef EVP_DYNAMOGRAPHINPUTS_HPP
#define EVP_DYNAMOGRAPHINPUTS_HPP

#include "Python/CommandCatalog.hpp"

namespace evp::dynamo {

CommandInfo LoaderCommand ();
bool PopulateLoaderInputs (const GS::UniString& graphPath, CommandInfo& command, GS::UniString& error);

} // namespace evp::dynamo

#endif
