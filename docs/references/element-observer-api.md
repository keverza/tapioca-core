# Archicad 29 – Element Observer & Change Notification API Reference

## Repository Disposition

- Authority: `AddOn/reference/archicad29-api-devkit/Support/Inc/ACAPinc.h`, `APIdefs_Callback.h`, and the DevKit notification examples.
- Conclusion: reference for notification experiments; the active change-tracking contract and write-free viewer policy remain in the API and Diligent specifications.

> Sourced verbatim from the AC29 API DevKit:
> - `Support/Inc/ACAPinc.h`
> - `Support/Inc/APIdefs_Callback.h`
> - `Examples/Notification_Manager/Src/*.cpp`

---

## Architecture

Two styles coexist:

| Style | Mechanism | Since | Key functions |
|-------|-----------|-------|---------------|
| **Old-style (C callback)** | Free-function callback registered per-addon | AC8+ | `ACAPI_Element_InstallElementObserver`, `ACAPI_Element_CatchNewElement`, `ACAPI_Element_AttachObserver` |
| **New-style (C++ interface)** | Subclass `API_IEventHandler` hierarchy, registered by ID | AC26+ | `ACAPI_Notification_RegisterEventHandler`, `ACAPI_Notification_UnregisterEventHandler` |

The old-style element observer chain works as follows:
1. Install a global callback via `ACAPI_Element_InstallElementObserver`.
2. Optionally, catch all newly created elements via `ACAPI_Element_CatchNewElement`.
3. Attach per-element observers via `ACAPI_Element_AttachObserver`.
4. The callback (`APIElementEventHandlerProc`) receives `const API_NotifyElementType *` with the event kind and element GUID.

---

## 1. Old-Style Element Observer APIs

### `ACAPI_Element_InstallElementObserver`

```cpp
// ACAPinc.h:7182
__APIEXPORT GSErrCode ACAPI_Element_InstallElementObserver (APIElementEventHandlerProc *handlerProc);
```

> This function is used to register/unregister an add-on which wants to monitor the changes in elements. This is a common callback for all the different element observers which can be attached to an element with `ACAPI_Element_AttachObserver`. After registration your add-on's handlerProc will be called when any of the monitored elements change.

Pass `nullptr` to unregister.

### `ACAPI_Element_CatchNewElement`

```cpp
// ACAPinc.h:7151
__APIEXPORT GSErrCode ACAPI_Element_CatchNewElement (
    const API_ToolBoxItem    *elemType,
    APIElementEventHandlerProc *handlerProc);
```

> This function enables the API tool add-on catch the event of creating a certain type of element. To receive notification on the creation of **any** type of elements, pass `nullptr` as `elemType`.

If you no longer need notifications, call again with `nullptr` in `handlerProc`.

### `ACAPI_Element_AttachObserver`

```cpp
// ACAPinc.h:3865
__APIEXPORT GSErrCode ACAPI_Element_AttachObserver (const API_Guid& elemGuid, GSFlags notifyFlags = 0);
```

> Attaches an observer to the given element. After attaching, the installed `APIElementEventHandlerProc` will be called with the appropriate notifications.

### `ACAPI_Element_DetachObserver`

```cpp
// ACAPinc.h:3877
__APIEXPORT GSErrCode ACAPI_Element_DetachObserver (const API_Guid& elemGuid);
```

### `ACAPI_Notification_GetObservedElements`

```cpp
// ACAPinc.h:3887
__APIEXPORT GSErrCode ACAPI_Notification_GetObservedElements (GS::Array<API_Elem_Head> *elemHeads);
```

### `ACAPI_Notification_GetTranParams`

```cpp
// ACAPinc.h:7197
__APIEXPORT GSErrCode ACAPI_Notification_GetTranParams (API_ActTranPars *actTranPars);
```

> The transformation data is valid only between `APINotifyElement_BeginEvents` and the following `APINotifyElement_EndEvents` notification.

### `ACAPI_Notification_GetParentElement`

```cpp
// ACAPinc.h:7216
__APIEXPORT GSErrCode ACAPI_Notification_GetParentElement (
    API_Element        *element,
    API_ElementMemo    *memo,
    UInt64              mask,
    API_ElementUserData *userData);
```

> Used to retrieve the original element data from which the notified element has been derived.

---

## 2. Callback Type & Notification Struct

### `APIElementEventHandlerProc`

```cpp
// APIdefs_Callback.h:812
typedef GSErrCode APIElementEventHandlerProc (const API_NotifyElementType* elemType);
```

> This is the function which will be called when your add-on attached an observer to an element with `ACAPI_Element_AttachObserver`, i.e. you are interested in changes to an element.

### `API_NotifyElementType`

```cpp
// APIdefs_Callback.h:447
struct API_NotifyElementType {
    API_ElementDBEventID  notifID;        // type of the notification
    Int32                 filler_1;
    API_Elem_Head         elemHead;       // element reference (type + guid)
    API_DatabaseUnId      databaseId;     // element database identifier
    Int32                 filler_2[8];
};
```

> In order to receive the notifications of created elements you should install an `APIElementEventHandlerProc` with the `ACAPI_Element_CatchNewElement` function. To be notified on deleting or modifying a specified element, attach an observer to the element you are interested in with `ACAPI_Element_AttachObserver`.
>
> **Do not modify the project database during Undo/Redo notifications.**
>
> Since each add-on can be notified twice during the same operation, the first call is marked with a -1 index value in the `APINotifyElement_BeginEvents` notification.

### `API_ElementDBEventID`

```cpp
// APIdefs_Callback.h:409
typedef enum {
    APINotifyElement_BeginEvents            = -1,
    APINotifyElement_EndEvents              = -2,

    APINotifyElement_New                    =  1,
    APINotifyElement_Copy                   =  2,
    APINotifyElement_Change                 =  3,
    APINotifyElement_Edit                   =  4,
    APINotifyElement_Delete                 =  5,

    APINotifyElement_Undo_Created           =  11,
    APINotifyElement_Undo_Modified          =  12,
    APINotifyElement_Undo_Deleted           =  13,
    APINotifyElement_Redo_Created           =  14,
    APINotifyElement_Redo_Modified          =  15,
    APINotifyElement_Redo_Deleted           =  16,
    APINotifyElement_PropertyValueChange    =  17,
    APINotifyElement_ClassificationChange   =  18
} API_ElementDBEventID;
```

---

## 3. Other Old-Style Notification APIs

### Project Events

```cpp
// ACAPinc.h:7069
__APIEXPORT GSErrCode ACAPI_ProjectOperation_CatchProjectEvent (
    GSFlags                       eventTypes,   // bitmask of API_NotifyEventID
    APIProjectEventHandlerProc   *handlerProc);
```

Callback typedef:
```cpp
// APIdefs_Callback.h:682-694
typedef GSErrCode APIProjectEventHandlerProc (API_NotifyEventID notifID, Int32 param);
```

Event masks (`APIdefs_Callback.h`):
```cpp
#define API_AllProjectNotificationMask    0xFFFFFFFF
#define API_AllTeamWorkNotificationMask   0x00018000   // SendChanges | ReceiveChanges
#define API_AllChangeNotificationMask     0x00460000   // ChangeProjectDB | ChangeWindow
#define API_AllNotificationMask           0xFFFFFFFF
```

### Defaults Changes

```cpp
// ACAPinc.h:7133
__APIEXPORT GSErrCode ACAPI_Element_CatchChangeDefaults (
    const API_ToolBoxItem          *elemType,     // nullptr = all tools
    APIDefaultsChangeHandlerProc   *handlerProc);
```

### Tool Change

```cpp
// ACAPinc.h:7102
__APIEXPORT GSErrCode ACAPI_Notification_CatchToolChange (
    APIToolChangeHandlerProc *handlerProc);
```

### Selection Change

```cpp
// ACAPinc.h:7116
__APIEXPORT GSErrCode ACAPI_Notification_CatchSelectionChange (
    APISelectionChangeHandlerProc *handlerProc);
```

### View Events (Project Navigator)

```cpp
// ACAPinc.h:7087
__APIEXPORT GSErrCode ACAPI_Notification_CatchViewEvent (
    GSFlags                    eventTypes,
    API_NavigatorMapID         mapId,
    APIViewEventHandlerProc   *handlerProc);
```

### Attribute Replacement

```cpp
// ACAPinc.h:7168
__APIEXPORT GSErrCode ACAPI_Notification_CatchAttributeReplacement (
    APIAttributeReplacementHandlerProc *handlerProc);
```

### Element Reservation (Teamwork)

```cpp
// ACAPinc.h:7235
__APIEXPORT GSErrCode ACAPI_Notification_CatchElementReservationChange (
    APIReservationChangeHandlerProc  *handlerProc,
    const GS::HashSet<API_Guid>      *filterElementsInterestedOnly = nullptr);
```

### Lockable Reservation (Teamwork)

```cpp
// ACAPinc.h:7252
__APIEXPORT GSErrCode ACAPI_Notification_CatchLockableReservationChange (
    APILockChangeHandlerProc         *handlerProc,
    const GS::HashSet<API_Guid>      *filterLockablesInterestedOnly = nullptr);
```

### Emit Visibility Notifications

```cpp
// ACAPinc.h:7275
__APIEXPORT GSErrCode ACAPI_Notification_ClassificationVisibilityChanged ();

// ACAPinc.h:7285
__APIEXPORT GSErrCode ACAPI_Notification_PropertyVisibilityChanged ();
```

---

## 4. New-Style C++ Interface (AC26+)

### `ACAPI_Notification_RegisterEventHandler` / `UnregisterEventHandler`

```cpp
// ACAPinc.h:7302
__APIEXPORT GSErrCode ACAPI_Notification_RegisterEventHandler (
    GS::Owner<API_IEventHandler> eventHandler,
    API_Guid& id);

// ACAPinc.h:7317
__APIEXPORT GSErrCode ACAPI_Notification_UnregisterEventHandler (const API_Guid& id);
```

> In case you register an event handler your add-on will automatically be kept loaded in memory.

### Base Classes (all from `APIdefs_Callback.h`)

```
API_IEventNotifier                          (AC26+)  — abstract notifier
  └─ API_IACEventNotifier                   (AC28+)  — dispatches to typed handlers

API_IEventHandler                           (AC26+)  — GetName() + Dispatch(notifier)

API_IObjectEventHandler                     (AC26+)  — OnCreated/Modified/Deleted(ids)

Concrete handlers you subclass:
  API_IAttributeEventHandler                (AC26+)  — attributes
  API_IAttributeFolderEventHandler          (AC26+)  — attribute folders
  API_IClassificationSystemEventHandler     (AC26+)  — classification systems
  API_IClassificationItemEventHandler       (AC26+)  — classification items
  API_IPropertyGroupEventHandler            (AC26+)  — property groups
  API_IPropertyDefinitionEventHandler       (AC26+)  — property definitions
  API_IGraphisoftIDEventHandler             (AC26+)  — OnUserChanged()
  API_IEnableAllInfoDialogEventHandler      (AC26+)  — EnableAllInfoDialog()
  API_IMarqueeEventHandler                  (AC26+)  — OnMarqueeChanged(selectionInfo)
  API_IWindowEventHandler                   (AC28+)  — OnWindowBroughtForward/SentBackward(window)
```

---

## 5. Verbose Usage Examples

### Example 1: Observe all newly created elements + attach observer on-the-fly

_Source: `Examples/Notification_Manager/Src/Element_Observer.cpp`_

```cpp
static bool allNewElements = false;

// --- The callback ---
GSErrCode ElementEventHandlerProc (const API_NotifyElementType *elemType)
{
    GSErrCode err = NoError;

    if (elemType->notifID == APINotifyElement_BeginEvents ||
        elemType->notifID == APINotifyElement_EndEvents)
    {
        API_DatabaseInfo api_dbPars {};
        api_dbPars.databaseUnId = elemType->databaseId;
        ACAPI_Window_GetDatabaseInfo (&api_dbPars);
        ACAPI_WriteReport (
            (elemType->notifID == APINotifyElement_BeginEvents)
                ? "Begin Events" : "End Events", false);
    }
    else
    {
        switch (elemType->notifID) {
            case APINotifyElement_New:
                if (allNewElements) {
                    // Attach observer to newly created element automatically
                    err = ACAPI_Element_AttachObserver (elemType->elemHead.guid);
                    if (err == APIERR_LINKEXIST)
                        err = NoError;
                }
                break;

            case APINotifyElement_Copy:
                if (allNewElements) {
                    err = ACAPI_Element_AttachObserver (elemType->elemHead.guid);
                    if (err == APIERR_LINKEXIST)
                        err = NoError;
                }
                break;

            case APINotifyElement_Change:
                // elemType->elemHead.guid identifies the changed element
                break;

            case APINotifyElement_Edit:
            {
                // Get transformation params and edit linked elements
                API_ActTranPars actTranPars {};
                ACAPI_Notification_GetTranParams (&actTranPars);
                // ... convert to API_EditPars, apply to linked elements ...
                break;
            }

            case APINotifyElement_Delete:
                // elemType->elemHead.guid was deleted
                break;

            case APINotifyElement_Undo_Created:
            case APINotifyElement_Undo_Modified:
            case APINotifyElement_Undo_Deleted:
            case APINotifyElement_Redo_Created:
            case APINotifyElement_Redo_Modified:
            case APINotifyElement_Redo_Deleted:
                // Do NOT modify the project database here
                break;

            case APINotifyElement_PropertyValueChange:
                break;

            case APINotifyElement_ClassificationChange:
                break;
        }
    }
    return err;
}

// --- Start observing all new elements ---
void Do_ElementMonitor (bool switchOn)
{
    if (switchOn) {
        ACAPI_Element_CatchNewElement (nullptr, ElementEventHandlerProc);
        ACAPI_Element_InstallElementObserver (ElementEventHandlerProc);
        allNewElements = true;
    } else {
        ACAPI_Element_CatchNewElement (nullptr, nullptr);
        ACAPI_Element_InstallElementObserver (nullptr);
        allNewElements = false;
    }
}
```

### Example 2: Observe a single user-clicked element

_Source: `Examples/Notification_Manager/Src/Element_Observer.cpp`_

```cpp
void Do_ClickedElementMonitor (bool switchOn)
{
    API_Guid elemGuid;
    if (!ClickAnElem ("Click an element", API_ZombieElemID, nullptr, nullptr, &elemGuid)) {
        WriteReport_Alert ("No element was clicked");
        return;
    }

    if (switchOn) {
        ACAPI_Element_InstallElementObserver (ElementEventHandlerProc);
        GSErrCode err = ACAPI_Element_AttachObserver (elemGuid);
        if (err == APIERR_LINKEXIST)
            err = NoError;
    } else {
        ACAPI_Element_DetachObserver (elemGuid);
    }
}
```

### Example 3: List all currently observed elements

_Source: `Examples/Notification_Manager/Src/Element_Observer.cpp`_

```cpp
void Do_ListMonitoredElements (void)
{
    GS::Array<API_Elem_Head> ppHeads;
    GSErrCode err = ACAPI_Notification_GetObservedElements (&ppHeads);
    if (err != NoError) {
        ErrorBeep ("ACAPI_Notification_GetObservedElements", err);
        return;
    }

    for (auto& refHead : ppHeads) {
        WriteReport ("%s guid=%s",
            ElemID_To_Name (refHead.type).ToCStr (CC_UTF8).Get (),
            APIGuidToString (refHead.guid).ToCStr ().Get ());
    }
}
```

### Example 4: Catch project events & auto-observe walls on open

_Source: `Examples/Notification_Manager/Src/Project_Observer.cpp`_

```cpp
static GSErrCode ProjectEventHandlerProc (API_NotifyEventID notifID, Int32 param)
{
    switch (notifID) {
        case APINotify_Open:
        {
            GS::Array<API_Guid> wallList;
            ACAPI_Element_GetElemList (API_WallID, &wallList);
            if (!wallList.IsEmpty ()) {
                ACAPI_Element_InstallElementObserver (ElementEventHandlerProc);
                for (const API_Guid& wallGuid : wallList) {
                    ACAPI_Element_AttachObserver (wallGuid);
                }
            }
            break;
        }
        case APINotify_New:
        case APINotify_Save:
        case APINotify_Close:
        case APINotify_Quit:
        case APINotify_ChangeProjectDB:
        case APINotify_ChangeWindow:
        case APINotify_ChangeFloor:
        // ... handle other events ...
        default:
            break;
    }
    return NoError;
}

void Do_CatchProjectEvent (bool switchOn)
{
    if (switchOn)
        ACAPI_ProjectOperation_CatchProjectEvent (API_AllNotificationMask, ProjectEventHandlerProc);
    else
        ACAPI_ProjectOperation_CatchProjectEvent (API_AllNotificationMask, nullptr);
}
```

### Example 5: Monitor selection changes

_Source: `Examples/Notification_Manager/Src/Selection_Observer.cpp`_

```cpp
static GSErrCode SelectionChangeHandlerProc (const API_Neig* selElemNeig)
{
    if (selElemNeig->neigID != APINeig_None) {
        char msgStr[256];
        sprintf (msgStr, "Last selected element: NeigID %d; guid: %s, inIndex: %d",
            selElemNeig->neigID,
            (const char *) APIGuid2GSGuid (selElemNeig->guid).ToUniString ().ToCStr (),
            selElemNeig->inIndex);
        ACAPI_WriteReport (msgStr, false);
    } else {
        ACAPI_WriteReport ("All elements deselected", false);
    }
    return NoError;
}

void Do_SelectionMonitor (bool switchOn)
{
    if (switchOn)
        ACAPI_Notification_CatchSelectionChange (SelectionChangeHandlerProc);
    else
        ACAPI_Notification_CatchSelectionChange (nullptr);
}
```

### Example 6: Monitor defaults changes

_Source: `Examples/Notification_Manager/Src/Default_Observer.cpp`_

```cpp
GSErrCode DefaultChangeHandlerProc (const API_ToolBoxItem *defElemType)
{
    char msgStr[256], elemStr[32];
    if (GetElementTypeString (defElemType->type, elemStr))
        sprintf (msgStr, "%s element type defaults changed", elemStr);
    else
        sprintf (msgStr, "Unknown element type defaults changed");
    ACAPI_WriteReport (msgStr, false);
    return NoError;
}

void Do_DefaultMonitor (bool switchOn)
{
    if (switchOn)
        ACAPI_Element_CatchChangeDefaults (nullptr, DefaultChangeHandlerProc);
    else
        ACAPI_Element_CatchChangeDefaults (nullptr, nullptr);
}
```

### Example 7: New-style – Attribute event handlers (AC26+)

_Source: `Examples/Notification_Manager/Src/Attribute_Observer.cpp`_

```cpp
GS::Optional<API_Guid> attributeEventHandlerId;
GS::Optional<API_Guid> attributeFolderEventHandlerId;

static GSErrCode RegisterAttributeHandler ()
{
    class AttributeEventHandler : public API_IAttributeEventHandler {
    public:
        virtual void OnCreated (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Attributes were created:", ids);
        }
        virtual void OnModified (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Attributes were modified:", ids);
        }
        virtual void OnDeleted (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Attributes were deleted:", ids);
        }
    };

    attributeEventHandlerId.New ();
    const auto result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<AttributeEventHandler> (), *attributeEventHandlerId);
    if (result != NoError) {
        attributeEventHandlerId.Clear ();
    }
    return result;
}

static GSErrCode RegisterAttributeFolderHandler ()
{
    class AttributeFolderEventHandler : public API_IAttributeFolderEventHandler {
    public:
        virtual void OnCreated (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
        virtual void OnModified (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
        virtual void OnDeleted (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
    };

    attributeFolderEventHandlerId.New ();
    const auto result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<AttributeFolderEventHandler> (), *attributeFolderEventHandlerId);
    if (result != NoError) {
        attributeFolderEventHandlerId.Clear ();
    }
    return result;
}

GSErrCode UnRegisterAttributeHandlers (void)
{
    if (attributeEventHandlerId.HasValue ()) {
        ACAPI_Notification_UnregisterEventHandler (*attributeEventHandlerId);
        attributeEventHandlerId.Clear ();
    }
    if (attributeFolderEventHandlerId.HasValue ()) {
        ACAPI_Notification_UnregisterEventHandler (*attributeFolderEventHandlerId);
        attributeFolderEventHandlerId.Clear ();
    }
    return NoError;
}
```

### Example 8: New-style – Property event handlers (AC26+)

_Source: `Examples/Notification_Manager/Src/Property_Observer.cpp`_

```cpp
GS::Optional<API_Guid> PropertyGroupEventHandlerId;
GS::Optional<API_Guid> PropertyDefinitionEventHandlerId;

static GSErrCode RegisterPropertyGroupHandler ()
{
    class PropertyGroupEventHandler : public API_IPropertyGroupEventHandler {
    public:
        virtual void OnCreated (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Property Groups were created:", ids);
        }
        virtual void OnModified (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Property Groups were modified:", ids);
        }
        virtual void OnDeleted (const GS::HashSet<API_Guid>& ids) const override
        {
            WriteReport ("The following Property Groups were deleted:", ids);
        }
    };

    PropertyGroupEventHandlerId.New ();
    const auto result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<PropertyGroupEventHandler> (), *PropertyGroupEventHandlerId);
    if (result != NoError) {
        PropertyGroupEventHandlerId.Clear ();
    }
    return result;
}

static GSErrCode RegisterPropertyDefinitionHandler ()
{
    class PropertyDefinitionEventHandler : public API_IPropertyDefinitionEventHandler {
    public:
        virtual void OnCreated (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
        virtual void OnModified (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
        virtual void OnDeleted (const GS::HashSet<API_Guid>& ids) const override { /*...*/ }
    };

    PropertyDefinitionEventHandlerId.New ();
    const auto result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<PropertyDefinitionEventHandler> (), *PropertyDefinitionEventHandlerId);
    if (result != NoError) {
        PropertyDefinitionEventHandlerId.Clear ();
    }
    return result;
}

GSErrCode UnRegisterPropertyEventHandlers (void)
{
    if (PropertyDefinitionEventHandlerId.HasValue ()) {
        ACAPI_Notification_UnregisterEventHandler (*PropertyDefinitionEventHandlerId);
        PropertyDefinitionEventHandlerId.Clear ();
    }
    if (PropertyGroupEventHandlerId.HasValue ()) {
        ACAPI_Notification_UnregisterEventHandler (*PropertyGroupEventHandlerId);
        PropertyGroupEventHandlerId.Clear ();
    }
    return NoError;
}
```

### Example 9: New-style – Marquee event handler (AC26+)

_Source: `Examples/Notification_Manager/Src/Marquee_Observer.cpp`_

```cpp
static std::optional<API_Guid> marqueeEventHandlerId;

GSErrCode RegisterMarqueeEventHandler ()
{
    class MarqueeEventHandler : public API_IMarqueeEventHandler {
    public:
        virtual void OnMarqueeChanged (const API_SelectionInfo& selectionInfo) const override
        {
            if (selectionInfo.typeID == API_SelEmpty) {
                ACAPI_WriteReport ("MarqueeEventHandler: Marquee has disappeared", false);
            } else {
                ACAPI_WriteReport ("MarqueeEventHandler: Marquee has changed", false);
            }
        }
    };

    API_Guid handlerId;
    const auto result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<MarqueeEventHandler> (), handlerId);
    if (result == NoError) {
        marqueeEventHandlerId = handlerId;
    }
    return result;
}

GSErrCode UnregisterMarqueeEventHandler ()
{
    if (marqueeEventHandlerId.has_value ()) {
        ACAPI_Notification_UnregisterEventHandler (*marqueeEventHandlerId);
        marqueeEventHandlerId.reset ();
    }
    return NoError;
}
```

### Example 10: New-style – Window event handler (AC28+)

_Source: `Examples/Notification_Manager/Src/Window_Observer.cpp`_

```cpp
GS::Optional<API_Guid> windowChangedEventHandlerId;

static void RegisterEventHandler ()
{
    class WindowEventHandler : public API_IWindowEventHandler {
    public:
        virtual void OnWindowBroughtForward (const API_WindowInfo& window) const override
        {
            char msgStr[256];
            sprintf (msgStr, "OnWindowBroughtForward: window typeID %d", window.typeID);
            ACAPI_WriteReport (msgStr, false);
        }

        virtual void OnWindowSentBackward (const API_WindowInfo& window) const override
        {
            char msgStr[256];
            sprintf (msgStr, "OnWindowSentBackward: window typeID %d", window.typeID);
            ACAPI_WriteReport (msgStr, false);
        }
    };

    windowChangedEventHandlerId.New ();
    GSErrCode result = ACAPI_Notification_RegisterEventHandler (
        GS::NewOwned<WindowEventHandler> (), *windowChangedEventHandlerId);
    if (result != NoError) {
        windowChangedEventHandlerId.Clear ();
    }
}

static void UnRegisterEventHandler ()
{
    if (windowChangedEventHandlerId.HasValue ()) {
        ACAPI_Notification_UnregisterEventHandler (*windowChangedEventHandlerId);
        windowChangedEventHandlerId.Clear ();
    }
}
```

---

## Quick Reference: Element Observer Lifecycle

```
// INITIALIZE (e.g. in RegisterInterface or on user action):

ACAPI_Element_InstallElementObserver (ElementEventHandlerProc);

// Option A – catch ALL new elements (nullptr = any type):
ACAPI_Element_CatchNewElement (nullptr, ElementEventHandlerProc);

// Option B – catch new elements of one type:
API_ToolBoxItem wallItem = { API_WallID };
ACAPI_Element_CatchNewElement (&wallItem, ElementEventHandlerProc);

// Attach to existing elements:
ACAPI_Element_AttachObserver (someWallGuid);

// Optionally, auto-attach from within the handler on APINotifyElement_New:
//   ACAPI_Element_AttachObserver (elemType->elemHead.guid);

// ---

// TEARDOWN (reverse order):

ACAPI_Element_DetachObserver (someWallGuid);
ACAPI_Element_CatchNewElement (nullptr, nullptr);       // or &wallItem, nullptr
ACAPI_Element_InstallElementObserver (nullptr);
```

