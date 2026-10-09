#ifndef EVP_METADATA_METADATASTORAGE_HPP
#define EVP_METADATA_METADATASTORAGE_HPP

// Metadata/MetadataStorage -- where Tapioca's metadata (TapiocaMetadata.hpp) lives in the
// project (the user, 2026-10-03): each element's on the element itself, as its user data; the
// project's schema in one unique Add-On Object.
//
// ⚠️ ON THE ELEMENT, SO IT FOLLOWS THE ELEMENT. User data goes with the element through undo,
// deletion and Teamwork reservation. Where a copy carries its original's -- the AC29 DevKit
// no longer defines the flag that asked for it, so that is Archicad's to decide, unmeasured --
// `Read` notices (the GUID it was written on differs) and makes it the copy's own
// (metadata::AdoptElement), in memory until it is next written. Tapioca's signature: the user
// data is this add-on's, a magic and the JSON; flags none.
//
// ⚠️ ONE SCHEMA PER PROJECT, A UNIQUE ADD-ON OBJECT ("TapiocaProjectSchema"). Absent, the
// project reads as `DefaultSchema` and nothing is written: reading never writes. The first
// write creates it -- in Teamwork that is a full send and receive, so it happens only when the
// user changes the schema; a write to one another user holds is refused with that reason.
//
// ⚠️ WRITES ARE DATABASE WRITES, INSIDE AN UNDO SCOPE THE CALLER OPENED: a WriteCommand's
// dispatcher, or ACAPI_CallUndoableCommand around a HUD edit. Never one of their own -- undo
// scopes do not nest (NativeCommands/CommandBase.hpp).
//
// MAIN THREAD, every entry point: all of it is ACAPI.

#include "Metadata/TapiocaMetadata.hpp"

#include <string>
#include <vector>

namespace geomsrv {
namespace metadata {
namespace storage {

// The element `guid`'s metadata (an Archicad GUID string). `present` false when it has none --
// an ordinary answer, `meta` then empty and the element's. False with `error` when the element
// cannot be read or its user data is not Tapioca's readable JSON.
bool Read (const std::string& guid, EntityMetadata& meta, bool& present, std::string& error);

// `meta` stored on the element `guid`, its GUID and entity id made the element's first. Inside an
// undo scope.
bool Write (const std::string& guid, EntityMetadata meta, std::string& error);

// The project's schema: the stored one -- extended with what the default has that it lacks, in
// memory -- or the default (`stored` false). False with `error` only when one is stored and
// cannot be read.
bool ReadSchema (ProjectSchema& schema, bool& stored, std::string& error);

// `schema` stored as the project's, its revision moved on. Inside an undo scope.
bool WriteSchema (ProjectSchema schema, std::string& error);

// The name the schema's Add-On Object goes by.
constexpr char kSchemaObjectName[] = "TapiocaProjectSchema";

// Text kept in the project's unique Add-On Object `name`, beside the schema (the flat
// programme). `stored` false when there is none -- reading never writes. False with `error`
// when one exists and cannot be read or is not Tapioca's.
bool ReadObject (const char* name, std::string& text, bool& stored, std::string& error);

// `text` stored as the Add-On Object `name`, created on the first write (in Teamwork, a send
// and receive). Inside an undo scope.
bool WriteObject (const char* name, const std::string& text, std::string& error);

// The Massing flat programme (floorprogramme::ToText).
constexpr char kProgrammeObjectName[] = "TapiocaFlatProgramme";

} // namespace storage
} // namespace metadata
} // namespace geomsrv

#endif
