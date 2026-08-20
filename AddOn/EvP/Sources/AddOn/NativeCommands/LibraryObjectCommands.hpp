#ifndef EVP_NATIVECOMMANDS_LIBRARYOBJECTCOMMANDS_HPP
#define EVP_NATIVECOMMANDS_LIBRARYOBJECTCOMMANDS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

// One row of the loaded-library catalogue, as ListLibraryParts reports it and as
// the palette's evp.LibraryPart browser shows it.
//
// ⚠️ `name` IS NOT AN IDENTITY. API_LibPart's own header says a document name is
// unique only in the sense that Archicad registers the NEWEST part carrying it —
// two loaded libraries routinely ship the same part, and the loser is simply
// invisible. `unID` (API_LibPart::ownUnID) is what stays put across sessions and
// across a library reload, so that is what a placement should carry. Both travel
// together because a unID never appears anywhere in Archicad's UI: a message
// quoting one tells the user nothing.
struct LibraryPartEntry {
    GS::UniString name;     // docu_UName — what the Object tool's dialog shows
    GS::UniString file;     // file_UName — the .gsm on disk
    GS::UniString unID;     // ownUnID — the stable cross-session key
    GS::UniString type;     // "Object", "Lamp", … — see LibPartTypeName
    GS::UniString location; // full path of the file, "" when unknown
    bool placeable = false;
    bool missing = false; // registered, but its .gsm is gone

    GS::UniString library; // the loaded library's name, "" if unknown
    bool embedded = false; // in the project's Embedded Library

    // Where the part sits in the tree Archicad's own Object Settings shows:
    //
    //     Loaded Libraries / Object Library / 1. BASIC LIBRARY / 1.1 Furnishing / Chairs
    //
    // WITHOUT the part's own name and WITHOUT its .gsm file node on the end.
    //
    // ⚠️ THIS IS "FOLDER VIEW", NOT "FOLDER VIEW (WITH SOURCES)" — Archicad's own
    // gear menu offers both, and LibraryTreePath::GetParts returns the WITH-
    // SOURCES one. It BEGINS with the physical package the part was shipped in
    // ("Fire Protection.libpack", "Archicad Library 27.lcf", "Grid Tool.apx"),
    // and dropping that leading node is precisely what MERGES the packages: a
    // dozen .libpacks all publish into the one "Object Library", so keeping the
    // container yields a dozen unrelated roots holding one library each, while
    // dropping it lets their folders fall together. The [Embedded|Loaded] root
    // is prepended in its place.
    //
    // ⚠️ AND THE PATH ENDS WITH THE .gsm FILE, which is NOT a folder. Left on, it
    // gave every object a folder of its own named "Armchair 01.gsm" containing
    // exactly one row — the user's words: *"I have open .gsm folder which always
    // has only one element just select it."* Both the file node and the part
    // node are stripped.
    //
    // ⚠️ THIS IS THE ONE THING THE LEGACY API CANNOT ANSWER, and why
    // ACAPI::Library is reached for at all. API_LibPart has a filesystem path
    // and nothing else, and a filesystem path is not this tree.
    GS::Array<GS::UniString> treePath;
};

// The catalogue behind evp.LibraryPart, narrowed to what can actually be PLACED.
//
// ⚠️ `subtypeFilter` EMPTY MEANS PLACEABLE OBJECTS, NOT EVERYTHING. That is a
// deliberate reversal of the first cut, which listed every registered library
// part and put surfaces, images, lamps, section markers and templates in a
// picker whose whole job is "choose a thing to place" — the user's words were
// *"data is all over the place"*. Archicad's own Object Settings browser lists
// GDL OBJECTS, and this matches it.
//
// Accepted spellings: "Object" (the default), "Lamp", "ZoneStamp", "Label", and
// "all" for the unfiltered catalogue. "Door", "Window" and "Skylight" are
// REFUSED rather than served: an opening belongs to the wall or roof it is cut
// into, so choosing one needs a picker with that context, and a list that merely
// names doors would be a working-looking control that cannot do the job.
//
// MAIN THREAD ONLY — an ACAPI enumeration. Shared with Palette's CatalogBrowser
// rather than round-tripped through the bus: the palette is already on the main
// thread, and a picker that had to call its own add-on to fill a list would be
// paying the gate for nothing.
//
// Returns APIERR_BADPARS for a refused or unknown subtype; `refusal` then holds
// the sentence to show the user.
GSErrCode CollectLibraryParts (const GS::UniString& subtypeFilter, GS::Array<LibraryPartEntry>& parts,
                               GS::UniString& refusal);

// The name a subtype filter is spelled with, for one API_LibTypeID. Returns
// "Other" for the values that have no agreed spelling.
GS::UniString LibPartTypeName (API_LibTypeID typeID);

// One library part's preview picture, decoded and ready to draw. False when the
// part has no preview, or has one this cannot read.
//
// ⚠️ NOT EVERY PART HAS A DECODABLE PREVIEW, and that is measured, not feared.
// The section is the one the DevKit names API_SectInfoGIF, but a real Archicad 29
// library stores whatever it likes there and declares it in a leading MIME
// string. Sampling the stock library on 2026-08-17 found all three cases in the
// first six objects: `image/png` (fine), `image/tiff` (NewDisplay::NativeImage
// has no decoder — its Encoding enum is JPEG and PNG only) and none at all. So a
// thumbnail view MUST have a no-image cell; it is the common case, not the edge.
//
// MAIN THREAD ONLY, and it touches the library file — call it for the cells
// actually on screen, never for a whole catalogue.
bool LoadLibraryPartPreview (const GS::UniString& partName, NewDisplay::NativeImage& image);

// GDL library-part placement: PlaceLibraryObject, and the catalogue read
// ListLibraryParts that tells a command what is available to place.
//
// Split out of CreateCommands when the CreateMesh level-line fix pushed that file
// past its size ceiling — and the right seam anyway: placing a configured GDL
// object is a different job from extruding a structural element, and the two GDL
// helpers it needs (LayerNameToIndex, ApplyGdlParam) had no other caller.
//
// Returns this domain's commands in registry order.
NativeCommandRegistrations GetLibraryObjectCommandRegistrations ();

} // namespace geomsrv

#endif
