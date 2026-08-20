#ifndef EVP_ADDONVERSION_HPP
#define EVP_ADDONVERSION_HPP

// The product's identity, in one place.
//
// F7 (palette-ui-plan.md) split the two names that used to be one:
//
//   * EVP_PRODUCT_NAME is the USER-FACING name. It appears in the menu, the
//     palette caption, the About box and status text. It is the only spelling a
//     user ever sees, and it may change again.
//   * The INTERNAL name stays "EvP", deliberately and permanently: %LOCALAPPDATA%\EvP\,
//     the `evp` Python package, `namespace evp`, the `EvP.*` bus verbs, EvP.apx /
//     EvPPy.dll and every command folder under "Documents\EvP Commands". Renaming
//     any of those would break existing commands, dumps, lockfiles and doc paths
//     for no user-visible gain. If you are here because `import evp` looks
//     inconsistent with the product name — it is, on purpose. Leave it.
//
// Macros rather than constants because ADDON_VERSION is pasted into adjacent
// string literals in the /health JSON (HttpServer.cpp), which only works at
// preprocessing time. This header replaces the ADDON_VERSION that CMakeLists.txt
// used to -D into the target: a header is greppable and one edit, a compile
// definition was neither.
//
#define EVP_PRODUCT_NAME "Tapioca"
#define ADDON_VERSION "0.0.1"

#endif
