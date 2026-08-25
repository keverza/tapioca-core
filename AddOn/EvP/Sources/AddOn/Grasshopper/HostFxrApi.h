#ifndef EVP_GRASSHOPPER_HOSTFXRAPI_H
#define EVP_GRASSHOPPER_HOSTFXRAPI_H

// The handful of declarations from Microsoft's hostfxr.h / coreclr_delegates.h
// that loading one managed assembly actually needs.
//
// ⚠️ DECLARED HERE RATHER THAN VENDORED ON PURPOSE. The alternative is nethost:
// a header, an import library and a nethost.dll to redistribute, added to
// AddOn/reference/CATALOG.yaml, for one function (get_hostfxr_path) that reads
// the same registry key this file reads in twenty lines. The .NET hosting ABI
// these declarations describe is versioned and documented as stable
// (learn.microsoft.com/dotnet/core/tutorials/netcore-hosting), which is exactly
// the kind of contract it is safe to restate. The full upstream headers are the
// authority; if a signature here ever disagrees with them, they win.
//
// The two calling conventions are NOT the same and are not interchangeable:
// hostfxr's own exports are __cdecl, the runtime delegates it hands back are
// __stdcall. Getting that wrong is a corrupted stack on the first call, on x86
// silently and on x64 through the returned value.

#include <stdint.h>

// char_t is wchar_t on Windows. The add-on builds with /Zc:wchar_t-, so that is
// `unsigned short` here -- which is the same 16-bit type hostfxr expects, so the
// pointers pass straight through. See Python/PathUtils.cpp for the same note.
typedef wchar_t evp_char_t;

#define EVP_HOSTFXR_CALLTYPE __cdecl
#define EVP_CORECLR_DELEGATE_CALLTYPE __stdcall

// hostfxr_initialize_for_runtime_config: >= 0 is success. 1 means the runtime
// was ALREADY initialized in this process by someone else and 2 means it was,
// with different properties -- neither is an error, and both are worth logging,
// because in Archicad they would mean another add-on got there first.
#define EVP_HOSTFXR_SUCCESS_HOST_ALREADY_INITIALIZED 1
#define EVP_HOSTFXR_SUCCESS_DIFFERENT_RUNTIME_PROPERTIES 2

// hostfxr_delegate_type. Only the one entry this code asks for is named.
#define EVP_HDT_LOAD_ASSEMBLY_AND_GET_FUNCTION_POINTER 5

// The delegate_type_name that means "the method carries [UnmanagedCallersOnly],
// so there is no delegate type to name". It is a sentinel pointer value, not a
// string -- never dereference it.
#define EVP_UNMANAGEDCALLERSONLY_METHOD ((const evp_char_t*) -1)

typedef void (EVP_HOSTFXR_CALLTYPE* evp_hostfxr_error_writer_fn) (const evp_char_t* message);

typedef int32_t (EVP_HOSTFXR_CALLTYPE* evp_hostfxr_initialize_for_runtime_config_fn) (
    const evp_char_t* runtimeConfigPath, const void* parameters, void** hostContextHandle);
typedef int32_t (EVP_HOSTFXR_CALLTYPE* evp_hostfxr_get_runtime_delegate_fn) (void* hostContextHandle, int32_t type,
                                                                             void** delegatePtr);
typedef int32_t (EVP_HOSTFXR_CALLTYPE* evp_hostfxr_close_fn) (void* hostContextHandle);
typedef evp_hostfxr_error_writer_fn (EVP_HOSTFXR_CALLTYPE* evp_hostfxr_set_error_writer_fn) (
    evp_hostfxr_error_writer_fn writer);

typedef int32_t (EVP_CORECLR_DELEGATE_CALLTYPE* evp_load_assembly_and_get_function_pointer_fn) (
    const evp_char_t* assemblyPath, const evp_char_t* typeName, const evp_char_t* methodName,
    const evp_char_t* delegateTypeName, void* reserved, void** functionPointer);

#endif
