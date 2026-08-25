#ifndef EVP_GRASSHOPPER_HOSTAPI_H
#define EVP_GRASSHOPPER_HOSTAPI_H

// The EvP.apx <-> Tapioca.GrasshopperHost.dll contract. ONE binary boundary,
// written down once, in C.
//
// The rules it obeys are core/CLAUDE.md's cross-binary rules applied to a
// managed peer instead of a CPython one:
//   * POD only. Ints, UTF-16 pointers, and one versioned struct. No C++ types,
//     no exceptions, no ownership crossing.
//   * Every string the native side hands over is UTF-16 and stays alive for the
//     duration of the call, nothing longer. Every string coming back is COPIED
//     into a caller-owned buffer (TapiocaGhCopyLastMessage) — the managed heap
//     never owns memory the add-on has to free, and vice versa.
//   * Nothing managed calls ACAPI. There is no callback into Archicad in this
//     slice at all: it is start, stop, ask. That is deliberate — a callback is
//     the thing that must not outlive an unload, so P0 ships without one and
//     the seam that adds one arrives with the lifecycle evidence to justify it.
//
// ⚠️ THE ABI VERSION IS CHECKED ON BOTH SIDES. The .apx and the managed
// assembly are built together but DEPLOYED as two files, and a stale
// Tapioca.GrasshopperHost.dll left beside a new .apx is the exact failure this
// guards: without the check it is a silent wrong-struct read, with it the
// menu command reports "managed host ABI 2, add-on expects 3".

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump on ANY change to the struct, the entry-point set, or a code's meaning.
#define TAPIOCA_GH_ABI_VERSION 1

// The managed type and method names hostfxr resolves. Kept here rather than at
// the call site so the C# attribute and the C++ lookup can be diffed side by side.
#define TAPIOCA_GH_ASSEMBLY L"Tapioca.GrasshopperHost.dll"
#define TAPIOCA_GH_TYPE L"Tapioca.GrasshopperHost.Bootstrap, Tapioca.GrasshopperHost"

// Return codes. Ok is 0; everything else is a distinct, ACTIONABLE reason —
// "startup and licensing failures leave Archicad usable and return actionable
// diagnostics" is an acceptance criterion of PLAT-RHINO-INSIDE, and a single
// generic failure code cannot satisfy it.
enum TapiocaGhStatus {
    TapiocaGhStatus_Ok = 0,
    TapiocaGhStatus_AlreadyRunning = 1,      // a core is already up; not an error
    TapiocaGhStatus_StartInProgress = 2,     // another start is in flight
    TapiocaGhStatus_Terminal = 3,            // stopped once; this process cannot host again
    TapiocaGhStatus_NotRunning = 4,          // stop/query with nothing up
    TapiocaGhStatus_AbiMismatch = 5,         // the two halves disagree; see above
    TapiocaGhStatus_WrongThread = 6,         // not the recorded STA owner thread
    TapiocaGhStatus_RuntimeMissing = 7,      // no hostfxr / no .NET 8 runtime
    TapiocaGhStatus_AssemblyMissing = 8,     // the managed host was not deployed
    TapiocaGhStatus_RhinoMissing = 9,        // no Rhino 8 installation resolved
    TapiocaGhStatus_RhinoInitFailed = 10,    // resolver or RhinoCore construction failed
    TapiocaGhStatus_LicenceUnavailable = 11, // Rhino refused to run unlicensed
    TapiocaGhStatus_GrasshopperFailed = 12,  // core is up, Grasshopper is not
    TapiocaGhStatus_Faulted = 13,            // an exception crossed the managed boundary
};

// Mirrors evp::grasshopper::HostState. The managed session keeps its own copy
// and the native side COMPARES them rather than assuming they agree — a
// disagreement is the first symptom of a half-torn-down host.
enum TapiocaGhState {
    TapiocaGhState_NotStarted = 0,
    TapiocaGhState_Starting = 1,
    TapiocaGhState_Running = 2,
    TapiocaGhState_Stopping = 3,
    TapiocaGhState_Stopped = 4,
    TapiocaGhState_Failed = 5,
};

// Start flags.
#define TAPIOCA_GH_FLAG_LOAD_GRASSHOPPER 0x0001u // load the Grasshopper plug-in after the core
#define TAPIOCA_GH_FLAG_SHOW_EDITOR 0x0002u      // P1 only; P0 never sets it

// ⚠️ VERSIONED BY SIZE AS WELL AS BY NUMBER. structSize is written by the caller
// and checked by the callee, so a field appended by a future slice is detectable
// rather than read as garbage out of a shorter struct.
struct TapiocaGhStartRequest {
    uint32_t structSize;            // = sizeof (struct TapiocaGhStartRequest)
    uint32_t abiVersion;            // = TAPIOCA_GH_ABI_VERSION
    uint32_t flags;                 // TAPIOCA_GH_FLAG_*
    uint32_t reserved;              // 0; keeps the pointers 8-byte aligned on both sides
    const uint16_t* rhinoSystemDir; // UTF-16, NUL-terminated; NULL = let the resolver find Rhino
    const uint16_t* logPath;        // UTF-16, NUL-terminated; NULL = no managed log file
};

// The four entry points, as they are declared on the managed side. Native code
// calls them through pointers obtained from hostfxr; these typedefs are what
// keeps the two descriptions of each signature in one file.
//
//   Start  - idempotent by contract: a second call while Running returns
//            AlreadyRunning and does NOT construct a second RhinoCore.
//   Stop   - releases Grasshopper and disposes the core where the pinned Rhino
//            supports it. Returns Ok even when disposal is a no-op, so a
//            shutdown path can call it unconditionally.
//   State  - never fails, never throws, safe during teardown.
//   CopyLastMessage - copies the last diagnostic (success or failure) as UTF-16
//            into a caller-owned buffer. Returns the number of characters
//            written excluding the terminator, or the required capacity when
//            the buffer is too small. The managed side frees nothing.
typedef int32_t (*TapiocaGhStartFn) (const struct TapiocaGhStartRequest* request);
typedef int32_t (*TapiocaGhStopFn) (void);
typedef int32_t (*TapiocaGhStateFn) (void);
typedef int32_t (*TapiocaGhCopyLastMessageFn) (uint16_t* buffer, int32_t capacityChars);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
