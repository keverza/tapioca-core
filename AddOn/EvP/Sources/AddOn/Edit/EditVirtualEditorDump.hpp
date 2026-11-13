#ifndef TAPIOCA_EDIT_VIRTUAL_EDITOR_DUMP_HPP
#define TAPIOCA_EDIT_VIRTUAL_EDITOR_DUMP_HPP

// The document a virtual editor debug dump carries (HudVirtualEditorDebug.cpp writes it), read
// for the offline replay tests and the UI Playground's editor fixtures. Authored values are
// schema-checked without persistence's region/geometry acceptance gate: broken geometry is
// exactly what a dump preserves. Throws std::runtime_error on a malformed dump.
#include "Edit/EditLandscapeJsonInternal.hpp"
#include <stdexcept>
#include <string>

namespace tapioca::geometry {
inline constexpr char kVirtualEditorDumpFormat[] = "tapioca.virtual-editor.debug";

// What the dump's top level says beside the document, all optional except the document.
struct VirtualEditorDumpHeader {
    std::string error, message; // the session's terrain status and last message when it was written
};

inline void ReadVirtualEditorDump (const landscape_json::Reader& root, geomsrv::edit::Document& doc,
                                   VirtualEditorDumpHeader* header = nullptr)
{
    if (root.Field ("format").Text () != kVirtualEditorDumpFormat || root.Field ("version").Integer () != 1)
        throw std::runtime_error ("Unsupported virtual editor dump format/version");
    const auto document = root.Field ("document");
    const int version = document.Field ("version").Integer ();
    if ((version != 1 && version != 2) || document.Field ("mode").Text () != "virtual")
        throw std::runtime_error ("Expected a schema-v1 or schema-v2 virtual document");
    landscape_json::ReadDocument (document, doc, LandscapeStorage::Virtual);
    if (!root.Field ("hadLandscape").Flag ())
        doc.landscape.reset ();
    if (header && root.Has ("status")) {
        const auto status = root.Field ("status");
        if (status.Has ("error"))
            header->error = status.Field ("error").Text ();
        if (status.Has ("message"))
            header->message = status.Field ("message").Text ();
    }
}
} // namespace tapioca::geometry
#endif
