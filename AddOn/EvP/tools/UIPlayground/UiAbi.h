#pragma once
#include <stddef.h>
#include <stdint.h>

/* Tool-only ABI, never the EvP/EvPPy/Archicad ABI. UTF-8 JSON, POD, cdecl.
   The only persistent handle is opaque and is destroyed by its creating module.
   No ImGui pointer, STL value, exception, or module allocation crosses this ABI. */
#define TAPIOCA_UI_ABI_VERSION 1u
#ifdef TAPIOCA_UI_MODULE
#define TAPIOCA_UI_EXPORT __declspec (dllexport)
#else
#define TAPIOCA_UI_EXPORT
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct TapiocaUiHost {
    uint32_t size;
    void* (*alloc) (size_t bytes);
    void (*release) (void* memory);
} TapiocaUiHost;
typedef struct TapiocaUiFrame {
    float width, height, dpiScale;
    const char* status; /* borrowed UTF-8, valid only during Draw */
} TapiocaUiFrame;
typedef struct TapiocaUiResult {
    char* stateJson; /* host allocator, host releases, null when unchanged */
    uint32_t requestBuild;
} TapiocaUiResult;
TAPIOCA_UI_EXPORT uint32_t TapiocaUiAbiVersion (void);
TAPIOCA_UI_EXPORT void* TapiocaUiCreate (const TapiocaUiHost* host, const char* initialJson);
TAPIOCA_UI_EXPORT void TapiocaUiDestroy (void* ui);
TAPIOCA_UI_EXPORT int TapiocaUiDraw (void* ui, const TapiocaUiFrame* frame, TapiocaUiResult* result);
#ifdef __cplusplus
}
#endif
