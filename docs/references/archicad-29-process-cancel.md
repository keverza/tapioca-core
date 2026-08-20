# Archicad 29 Process Window and Cancellation Reference

## Repository Disposition

- Authority: Archicad 29 API DevKit headers, examples, and generated documentation under `AddOn/reference/archicad29-api-devkit/`.
- Conclusion: reference material only; verify process-window and execution-policy symbols against the local DevKit before implementation.

Based on the AC29 SDK documentation, here's the information extracted directly from the files:

---

## **How Background Tasks Are Run in AC29 SDK**

### **1. Process Window System**
The primary mechanism for running and managing background/long-running tasks is through the **Process Window API**:

- **`ACAPI_ProcessWindow_InitProcessWindow`** - Opens the process window for a long operation
  - Parameters: `title` (process name), `nPhase` (number of phases), `processControlType` (optional)
  - Returns: `NoError` or `APIERR_BADPARS` if nPhase is nullptr
  - From: `<repo>

- **`ACAPI_ProcessWindow_SetNextProcessPhase`** - Starts the next phase
  - Parameters: `subtitle` (phase description), `maxval` (max progress value), `showPercent` (optional)
  - From: `ACAPI_Interface.h:171-184`

- **`ACAPI_ProcessWindow_SetProcessValue`** - Sets absolute progress value
  - From: `ACAPI_Interface.h:187-199`

- **`ACAPI_ProcessWindow_IncProcessValue`** - Increments progress value
  - From: `ACAPI_Interface.h:202-212`

- **`ACAPI_ProcessWindow_CloseProcessWindow`** - Closes the process window
  - From: `ACAPI_Interface.h:159-168`

---

### **2. How to Check for Cancellation**

**Primary method:**
- **`ACAPI_ProcessWindow_IsProcessCanceled()`** - Checks if user canceled via Cancel button or keyboard
  - Returns: `NoError` (continue) or `APIERR_CANCEL` (user canceled)
  - **Remarks**: "This function is used to check whether the user has canceled a long process (by clicking on the Cancel button of the process window, or by pressing the appropriate keys on the keyboard). It also give visual feedback on processing by changing the cursor shape."
  - From: `ACAPI_Interface.h:215-226` and `docs\group___process_window.html:425-445`

**Alternative method via GS::ProcessControl:**
The `GS::ProcessControl` class (from `Support\Modules\GSRoot\GSProcessControl.hpp`) provides:
- `virtual bool TestBreak() = 0` - Check if break was requested
- `virtual bool WasBreak() const = 0` - Check if break already occurred
- `virtual bool ShouldAbort() noexcept = 0` - Check if should abort

You can obtain the process control object via:
- **`ACAPI_ProcessWindow_GetProcessControl(GS::ProcessControl** processControl)`** - Returns Archicad's actual process control
  - From: `ACAPI_Interface.h:120-124` and `docs\group___process_window.html:256-282`

**Example from ZoneBoundaryQuery_Test:**
```cpp
GS::ProcessControl* processControl = nullptr;
ACAPI_ProcessWindow_GetProcessControl (&processControl);
```

---

### **3. How to Avoid Freezing Archicad Process**

#### **Option A: Use Process Window with Regular Checks**
Call `ACAPI_ProcessWindow_IsProcessCanceled()` regularly in your long-running loop to:
- Allow Archicad to process events
- Update the UI
- Check for user cancellation

**From FAQ (`docs\_faq.html:393-398`):**
> *How can I open a progress window when I have a long process running?*
> In several ways:
> - use the set of functions (`ACAPI_ProcessWindow_InitProcessWindow`, etc.) provided by the API. This has a small problem: other processes within Archicad are using the same progress window, and they can interfere with the information displayed by your add-on.
> - use your own modeless palette as a process window.
> - **open a modal dialog, and drive the process from there. Since DevKit v4.1 you can switch databases even if a modal dialog is visible on the screen. See the `Do_ProgressWindow` function in the *DG_Test* example. This is the recommended solution at the moment.**

#### **Option B: Use TIWait Function**
When waiting for asynchronous responses or in loops:
- **`TIWait(double delaySeconds = 1.0)`** - Lets other threads run
- From: `Support\Modules\GSRoot\GSTime.hpp:231`

**From `ACAPinc.h:6625`:**
> "Please note that if you use a loop for waiting for the asynchronous responses, **do not reserve the processor time superfluously, let other threads run by calling the TIWait function of the GSRoot module**."

#### **Option C: Use Proper Execution Policy**
For add-on commands, use the appropriate execution policy:

From `APIdefs_Registration.h:720-734`:
```cpp
enum class API_AddOnCommandExecutionPolicy {
    /** Immediately executes the Add-On command on a parallel thread. */
    InstantExecutionOnParallelThread,  // Faster, but CANNOT modify Archicad database (query only)

    /** Schedules the Add-On command for execution on the main thread. */
    ScheduleForExecutionOnMainThread   // Full database access via ACAPI functions
};
```

**Remarks from `APIdefs_Registration.h:724-726`:**
> "The `InstantExecutionOnParallelThread` is faster than the `ScheduleForExecutionOnMainThread` mode, but it cannot modify the Archicad database and is useful for querying data. The `ScheduleForExecutionOnMainThread` has full access to the Archicad database via the ACAPI functions."

---

## **Complete Usage Pattern from SDK Documentation**

From `docs\group___process_window.html` example:

```cpp
GSErrCode       err;
API_LibPart     libPart {};
GS::UniString   title ("Listing the library");
GS::UniString   subtitle ("working...");
Int32           nPhase;
Int32           i, nLib;
char            buffer [256];

err = ACAPI_LibraryPart_GetNum (&nLib);
if (nLib > 0) {
    nPhase = 1;

    ACAPI_ProcessWindow_InitProcessWindow (&title, &nPhase);
    ACAPI_ProcessWindow_SetNextProcessPhase (&subtitle, &nLib);

    for (i = 1; i <= nLib; i++) {
        libPart.index = i;
        err = ACAPI_LibraryPart_Get (&libPart);
        if (!err) {
            sprintf (buffer, "[%2d] \"%s\"", i, (const char *) GS::UniString (libPart.docu_UName).ToCStr ());
            ACAPI_WriteReport (buffer, false);
        }

        ACAPI_ProcessWindow_SetProcessValue (&i);
        if (ACAPI_ProcessWindow_IsProcessCanceled ())
            break;
    }

    ACAPI_ProcessWindow_CloseProcessWindow ();
}
```

---

## **Summary Table**

| Aspect | API Function/Class | Location |
|--------|-------------------|----------|
| **Init process window** | `ACAPI_ProcessWindow_InitProcessWindow` | `ACAPI_Interface.h:156` |
| **Update phase** | `ACAPI_ProcessWindow_SetNextProcessPhase` | `ACAPI_Interface.h:184` |
| **Update progress** | `ACAPI_ProcessWindow_SetProcessValue` / `IncProcessValue` | `ACAPI_Interface.h:199, 212` |
| **Check cancellation** | `ACAPI_ProcessWindow_IsProcessCanceled` | `ACAPI_Interface.h:226` |
| **Get process control** | `ACAPI_ProcessWindow_GetProcessControl` | `ACAPI_Interface.h:124` |
| **Avoid freezing** | `TIWait()` | `GSTime.hpp:231` |
| **Thread execution** | `API_AddOnCommandExecutionPolicy` | `APIdefs_Registration.h:728` |
