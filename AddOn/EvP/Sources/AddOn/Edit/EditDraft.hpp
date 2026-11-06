#ifndef TAPIOCA_EDIT_DRAFT_HPP
#define TAPIOCA_EDIT_DRAFT_HPP

// Durable virtual-editor drafts (#159 D1). SDK-free; no file IO here.
//
// A draft is a versioned envelope around the full virtual landscape codec
// (EditLandscapeSerialization.hpp, LandscapeStorage::Virtual). The definition is kept as
// the codec's exact text, not re-encoded, so a draft restores exactly what the codec does:
// entities and their contours, paths, layers, groups, the landscape DAG, materials,
// controls, details and every stable ID. It stores authored truth only: no worker handle,
// derived mesh or undo history, and native bindings are what the codec already carries
// (a reopened draft is never treated as current native state; native writes stay #25's).
//
//   { "format": "tapioca.virtual-editor.draft", "version": 1,
//     "savedUtc": "...", "revision": "<doc revision at save>",
//     "session": "<landscape session uuid>", "definition": "<virtual codec text>" }
//
// A reader refuses another format, a newer version, a malformed envelope or a definition
// the codec refuses; the caller's document is untouched on every refusal.
#include "Edit/EditDocument.hpp"
#include <cstdint>
#include <string>

namespace tapioca::geometry {

inline constexpr const char* kDraftFormat = "tapioca.virtual-editor.draft";
inline constexpr int kDraftVersion = 1;
inline constexpr const char* kDraftExtension = ".tapdraft";

struct DraftInfo {
    int version = 0;
    std::string savedUtc, session;
    uint64_t revision = 0;
};

// The envelope for `doc` (which must hold a landscape and validate). `savedUtc` is the
// caller's clock, so the text is deterministic for tests.
bool WriteDraft (const geomsrv::edit::Document& doc, const std::string& savedUtc, std::string& text,
                 std::string& error);
// Reads into `doc` only when the whole draft is valid.
bool ReadDraft (const std::string& text, geomsrv::edit::Document& doc, DraftInfo& info, std::string& error);

// A file name for a draft the user named: letters, digits, space, '-', '_' and '.' kept,
// anything else (path separators included) replaced by '_', trimmed, at most 80 characters,
// with the draft extension. Empty when nothing usable is left.
std::string DraftFileName (const std::string& name);

} // namespace tapioca::geometry
#endif
