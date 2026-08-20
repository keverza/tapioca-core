#ifndef EVP_NATIVECOMMANDS_LAYOUTCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_LAYOUTCOMMANDS_HPP

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// The Layout Book / independent-database domain: creating and deleting the
// CONTAINERS a drawing gets placed into — worksheets, details, layouts, master
// layouts, 3D documents — plus Layout Book subsets. ListDatabases / ListViews
// (read), CreateDatabase / DeleteDatabase / SetDocumentFrom3DSettings
// (structural). Registrations retain registry order.
//
// ⚠️ Everything here except the read is a StructuralCommand, NOT a
// WriteCommand: these ACAPI calls are non-undoable and refuse to run inside an
// undo scope. Read the StructuralCommand comment in CommandBase.hpp before
// adding to this file.
NativeCommandRegistrations GetLayoutCommandRegistrations ();

} // namespace geomsrv

#endif
