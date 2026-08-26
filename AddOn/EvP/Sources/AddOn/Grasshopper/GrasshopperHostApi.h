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
//   * The managed half reaches Archicad through ONE native function and nothing
//     else. P0 shipped with no managed-to-native direction at all, deliberately,
//     because a callback is the thing that must not outlive an unload. ABI 4
//     adds one, with the evidence that was asked for: an in-process Grasshopper
//     component solves on Archicad's main thread, which is the only thread ACAPI
//     may be touched from, so a direct call is both possible and the ONLY thing
//     that works here — the HTTP route is dead in this process (see
//     TapiocaGhNativeApi below for the measurement). The pointer is revoked at
//     Stop, which is what keeps it from outliving the add-on.
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
// 2: added ShowEditor/HideEditor for the Grasshopper Editor command (slice 1).
// 3: added archicadJsonPort to the start request (slice 1, Tapir).
// 4: added nativeApi — the FIRST managed-to-native direction. See below.
#define TAPIOCA_GH_ABI_VERSION 4

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
    TapiocaGhStatus_EditorUnavailable = 14,  // the core is up but Grasshopper's editor is not
    TapiocaGhStatus_BufferTooSmall = 15,     // the response did not fit; ask again with neededChars
    TapiocaGhStatus_UnknownCommand = 16,     // no native command by that name
    TapiocaGhStatus_BadRequest = 17,         // the parameters were not JSON, or an argument was null
    TapiocaGhStatus_CommandFailed = 18,      // the command ran and reported a failure
    TapiocaGhStatus_WriteRefused = 19,       // a write command; see TapiocaGhCallNativeFn
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

struct TapiocaGhNativeApi;

// ⚠️ VERSIONED BY SIZE AS WELL AS BY NUMBER. structSize is written by the caller
// and checked by the callee, so a field appended by a future slice is detectable
// rather than read as garbage out of a shorter struct.
struct TapiocaGhStartRequest {
    uint32_t structSize; // = sizeof (struct TapiocaGhStartRequest)
    uint32_t abiVersion; // = TAPIOCA_GH_ABI_VERSION
    uint32_t flags;      // TAPIOCA_GH_FLAG_*
    // THIS Archicad instance's JSON port, from ACAPI_Command_GetHttpConnectionPort;
    // 0 when it could not be determined.
    //
    // It is here rather than left to the Grasshopper side to work out because it
    // CANNOT be worked out there. Tapir's components default to 19723 and the
    // plugin has no Archicad-instance discovery at all, so with two Archicad
    // instances open the second one's definitions would silently drive the first
    // one's model. Only code running inside this process can answer "which
    // Archicad am I", and this is that answer, passed across once at startup.
    uint32_t archicadJsonPort;
    const uint16_t* rhinoSystemDir; // UTF-16, NUL-terminated; NULL = let the resolver find Rhino
    const uint16_t* logPath;        // UTF-16, NUL-terminated; NULL = no managed log file
    // The managed-to-native direction. NULL is legal and means "no direct
    // bridge"; the host still starts and the editor still opens.
    const struct TapiocaGhNativeApi* nativeApi;
};

// ⚠️ WHY THIS DIRECTION EXISTS AT ALL, AND WHY IT IS NOT AN HTTP CALL.
//
// Measured 2026-08-26, from inside a live Grasshopper solution: while a solve
// holds Archicad's main thread inside an add-on callback, Archicad executes NO
// JSON command that needs that thread — not Tapir's, not Tapioca's, not even
// Graphisoft's own API.GetSelectedElements, and not from a background thread
// either. Only API.IsAlive survives, because Archicad answers it on its HTTP
// server thread without scheduling anything. Pumping messages does not help; a
// callback cannot return to the loop it is standing on.
//
// The inverse of that constraint is this bridge. A Grasshopper component solves
// ON Archicad's main thread, which is exactly the thread ACAPI requires. So it
// does not need a socket, a scheduler or a wait: it can call straight through
// and get an answer in the same stack frame. What makes an HTTP bridge
// impossible in this process is precisely what makes a direct one trivial.
//
// The boundary stays POD: UTF-16 in, UTF-16 into a CALLER-OWNED buffer out, an
// int32 status back. No ownership crosses; the add-on frees nothing the managed
// side allocated and vice versa.
//
// ⚠️ MAIN THREAD ONLY, AND IT IS CHECKED. A call from any other thread returns
// TapiocaGhStatus_WrongThread and does not touch ACAPI. That check is not
// defensive tidiness: a Grasshopper component that quietly moved its work to a
// worker would otherwise corrupt Archicad instead of failing.
//
// ⚠️ READS ONLY IN THIS SLICE. A write command needs exactly one undo scope, and
// ExecuteNativeCommand deliberately opens none (see AddOnCommands.hpp). Rather
// than open one here and duplicate the transaction rules that already live in
// the command bus, a write is REFUSED by name with TapiocaGhStatus_WriteRefused.
// Writes arrive when the undo/transaction seam is designed, not before.
//
// Returns a TapiocaGhStatus. On Ok the response JSON is written to `buffer` with
// a NUL terminator and *neededChars is the length excluding it. When the buffer
// is too small nothing is written, BufferTooSmall is returned, and *neededChars
// carries the required length — so the caller sizes on the second attempt. Pass
// buffer = NULL with capacityChars = 0 to ask for the size alone.
typedef int32_t (*TapiocaGhCallNativeFn) (const uint16_t* commandName,
                                          const uint16_t* parametersJson,
                                          uint16_t*       buffer,
                                          int32_t         capacityChars,
                                          int32_t*        neededChars);

// The native functions handed to the managed half at start. Versioned by size
// like the request, so a later slice can append without a silent misread.
struct TapiocaGhNativeApi {
    uint32_t              structSize; // = sizeof (struct TapiocaGhNativeApi)
    uint32_t              abiVersion; // = TAPIOCA_GH_ABI_VERSION
    TapiocaGhCallNativeFn callNative;
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
//   ShowEditor / HideEditor - the Grasshopper canvas, shown and hidden WITHOUT
//            touching the core. That separation is the whole point of slice 1:
//            the Editor is a window over a runtime that outlives it, so closing
//            the canvas must never dispose RhinoCore and re-opening it must
//            never build a second one. Both are idempotent; ShowEditor on a
//            visible editor and HideEditor on a hidden one each return Ok.
typedef int32_t (*TapiocaGhStartFn) (const struct TapiocaGhStartRequest* request);
typedef int32_t (*TapiocaGhStopFn) (void);
typedef int32_t (*TapiocaGhStateFn) (void);
typedef int32_t (*TapiocaGhCopyLastMessageFn) (uint16_t* buffer, int32_t capacityChars);
typedef int32_t (*TapiocaGhShowEditorFn) (void);
typedef int32_t (*TapiocaGhHideEditorFn) (void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
