# Archicad 29 Selection API Reference

## Repository Disposition

- Authority: Archicad 29 API DevKit headers and SDK examples under `AddOn/reference/archicad29-api-devkit/`.
- Conclusion: reference material for selection and highlight calls; verify the local headers before adding or changing a native command.

Here are the **exact usage methods** for element selection, highlight, and related functions from the **Archicad 29 API DevKit**:

---

---

## **📌 1. SELECTION FUNCTIONS**

---

### **🔹 ACAPI_Selection_Select**
**Purpose**: Add/remove elements to/from the current selection.

**Function Signature**:
```cpp
__APIEXPORT GSErrCode ACAPI_Selection_Select (const GS::Array<API_Neig>& selNeigs, bool add);
```

**Parameters**:
- `selNeigs`: [in] Array of `API_Neig` structures identifying elements/parts to select
- `add`: [in] `true` = add to selection, `false` = remove from selection

**Return Values**:
- `NoError` - The function has completed with success.
- `APIERR_BADDATABASE` - The current database is not proper for the operation.
- `APIERR_BADID` - The element unique ID is invalid. The element type is invalid, or the element type is not supported by the server application.
- `APIERR_BADPARS` - The passed parameter contains invalid data; selNeigs.

**Remarks**: 
You can use this function to add/remove (add flag) a number of elements to/from the current selection. The elements are defined by the selNeigs array of type `API_Neig`. Use `ACAPI_Selection_DeselectAll` function to remove all elements from the selection.
The neigID and the guid fields are required (inIndex and/or holeSel only where applicable).
The `API_NeigID` differs from the `API_ElemTypeID`, because it refers to the selectable parts of the elements, not the elements themselves.
You can select not only whole elements but element parts, such as vertices, edges and faces, specified in the `elemPartType` and `elemPartIndex` fields of `API_Neig`.

---

#### **✅ Exact Usage Examples from SDK:**

**Example 1** - Select clicked element (Selection_Manager.cpp:192-201):
```cpp
static void Do_SelectClickedElement (bool select)
{
    API_Neig clickedNeig;
    if (ClickAnElem ("Click an elem to select/deselect", API_ZombieElemID, &clickedNeig)) {
        GSErrCode err = ACAPI_Selection_Select ({ clickedNeig }, select);
        if (err != NoError)
            ErrorBeep ("ACAPI_Selection_Select", err);
    } else {
        WriteReport_Alert ("No element was clicked");
    }
}
```

**Example 2** - Select elements from marquee (Selection_Manager.cpp:95-102):
```cpp
err = ACAPI_Selection_Select (selNeigs, true);
if (err != NoError) {
    ErrorBeep ("ACAPI_Selection_Select", err);
}
if (err == APIERR_NOSEL)
    err = NoError;
```

**Example 3** - Single element selection (Browser_Control/Src/BrowserPalette.cpp:212):
```cpp
ACAPI_Selection_Select ({ API_Neig (APIGuidFromString (elemGuidStr.ToCStr ().Get ())) }, modification == AddToSelection);
```

**Example 4** - Select Morph face edges (Selection_Manager.cpp:345-347):
```cpp
err = ACAPI_Selection_Select (selNeigs, true);
if (err != NoError)
    ErrorBeep ("ACAPI_Selection_Select", err);
```

---

---

### **🔹 ACAPI_Selection_DeselectAll**
**Purpose**: Deselect all selected elements ("Deselect All").

**Function Signature**:
```cpp
__APIEXPORT GSErrCode ACAPI_Selection_DeselectAll ();
```

**Return Values**:
- `NoError` - The function has completed with success.
- `APIERR_BADDATABASE` - The current database is not proper for the operation.

**Remarks**: This function acts as "Deselect All". Use `ACAPI_Selection_Select` function to add/remove elements to/from the current selection.

---

#### **✅ Exact Usage Examples from SDK:**

**Example 1** - Clear selection before new selection (Selection_Manager.cpp:42-45):
```cpp
err = ACAPI_Selection_DeselectAll ();
if (err != NoError)
    ErrorBeep ("ACAPI_Selection_DeselectAll", err);
```

**Example 2** - Standalone deselect (Selection_Manager.cpp:209-217):
```cpp
static void Do_DeselectAll (void)
{
    GSErrCode err;
    err = ACAPI_Selection_DeselectAll ();
    if (err != NoError)
        ErrorBeep ("ACAPI_Selection_DeselectAll", err);
    return;
}
```

---

---

### **🔹 ACAPI_Selection_Get**
**Purpose**: Get information about current selection and selected elements.

**Function Signature**:
```cpp
__APIEXPORT GSErrCode ACAPI_Selection_Get (
    API_SelectionInfo*        selectionInfo,
    GS::Array<API_Neig>*    selNeigs,
    bool                    onlyEditable,
    bool                    ignorePartialSelection = true,
    API_SelRelativePosID    relativePosToMarquee = API_InsidePartially
);
```

**Parameters**:
- `selectionInfo`: [out] Information about selection type and marquee
- `selNeigs`: [out] Array of selected elements (can be nullptr)
- `onlyEditable`: [in] true = return only editable elements
- `ignorePartialSelection`: [in] true = ignore partial element selection (default: true)
- `relativePosToMarquee`: [in] Filter elements relative to marquee position (default: API_InsidePartially)

**Return Values**:
- `NoError` - The function has completed with success.
- `APIERR_NOPLAN` - There is no open project.
- `APIERR_NOSEL` - There is no selection. Note that this is not a real error!

**Remarks**: 
This function is used to get information about the current selection and to retrieve the selected elements. The information is returned in the selectionInfo parameter.
If individual elements are selected the `API_SelElems` identifier is returned. The number of the selected and selected and editable elements are also returned. In case of marquee based selection, the function gives back the actual polygon of the current selection in the `API_SelectionInfo` structure; don't forget to dispose this handle.
The `API_SelEmpty` identifier in the typeID field of the selectionInfo parameter means that no selection is actually used in Archicad.
In case of individually selected elements, Archicad returns all the elements which are selected. In the case of marquee based selection, only those will be returned which match the position criteria defined by relativePosToMarquee.

---

#### **✅ Exact Usage Examples from SDK:**

**Example 1** - Get selection with marquee (Selection_Manager.cpp:76-81):
```cpp
err = ACAPI_Selection_Get (&selectionInfo, &selNeigs, false, false, relativePos);
if (err != NoError) {
    ErrorBeep ("ACAPI_Selection_Get", err);
    return;
}
```

**Example 2** - Get current selection (Selection_Manager.cpp:120-125):
```cpp
err = ACAPI_Selection_Get (&selectionInfo, &selNeigs, false, false);
if (err != NoError) {
    ErrorBeep ("ACAPI_Selection_Get", err);
    return;
}
```

**Example 3** - Get selection for duplication (Selection_Manager.cpp:259-263):
```cpp
err = ACAPI_Selection_Get (&selectionInfo, &selNeigs, false);
if (err != NoError) {
    ErrorBeep ("ACAPI_Selection_Get", err);
    return err;
}
```

**Example 4** - With editable filter (Automate_Functions.cpp:311):
```cpp
API_SelectionInfo  selectionInfo;
GS::Array<API_Neig> selNeigs;
GSErrCode err = ACAPI_Selection_Get (&selectionInfo, &selNeigs, false);
```

**Example 5** - With cleanup (Browser_Control/Src/BrowserPalette.cpp:192):
```cpp
API_SelectionInfo  selectionInfo;
GS::Array<API_Neig> selNeigs;
ACAPI_Selection_Get (&selectionInfo, &selNeigs, false, false);
BMKillHandle ((GSHandle*)&selectionInfo.marquee.coords);
```

---

---

### **🔹 ACAPI_Selection_SetMarquee**
**Purpose**: Set/modify the current marquee selection.

**Function Signature**:
```cpp
__APIEXPORT GSErrCode ACAPI_Selection_SetMarquee (API_SelectionInfo *selectionInfo);
```

**Parameters**:
- `selectionInfo`: [in] Parameters of the marquee (typeID must be `API_MarqueePoly`, `API_MarqueeHorBox`, or `API_MarqueeRotBox`)

**Return Values**:
- `NoError` - The function has completed with success.
- `APIERR_BADPARS` - The selectionInfo parameter is nullptr
- `APIERR_BADDATABASE` - The active database is neither the floorplan, nor a section database

**Remarks**: 
This function is used to change the marquee on a floorplan or section window. The typeID field of the selectionInfo must be `API_MarqueePoly`, `API_MarqueeHorBox` or `API_MarqueeRotBox` in order to set a new marquee outline, otherwise the actual marquee will be removed.
The function has no effect on the individual selection of elements.
In case of `API_MarqueePoly` type marquee do not forget to release the coordinate handle passed in the selectionInfo parameter.

---

#### **✅ Exact Usage Examples from SDK:**

**Example** - Set polygon marquee (Selection_Manager.cpp:47-74):
```cpp
API_SelectionInfo selectionInfo {};
GS::Array<API_Neig> selNeigs;
API_ElemType type;
GSErrCode err;

// Setup marquee polygon
selectionInfo.typeID = API_MarqueePoly;
selectionInfo.marquee.nCoords = 10;
selectionInfo.marquee.coords = (API_Coord **) BMAllocateHandle (
    selectionInfo.marquee.nCoords * sizeof (API_Coord), ALLOCATE_CLEAR, 0
);
if (selectionInfo.marquee.coords) {
    API_Coord *coords = *selectionInfo.marquee.coords;
    coords[0].x = coords[0].y = 0.0;
    coords[1].x = 2.0;  coords[1].y = -2.0;
    coords[2].x = 4.0;  coords[2].y = 0.0;
    coords[3].x = 4.0;  coords[3].y = 2.0;
    coords[4].x = 8.0;  coords[4].y = 2.0;
    coords[5].x = 8.0;  coords[5].y = 6.0;
    coords[6].x = 4.0;  coords[6].y = 6.0;
    coords[7].x = 4.0;  coords[7].y = 4.0;
    coords[8].x = 0.0;  coords[8].y = 4.0;
    coords[9] = coords[0];

    selectionInfo.multiStory = true;

    err = ACAPI_Selection_SetMarquee (&selectionInfo);
    BMKillHandle ((GSHandle *) &selectionInfo.marquee.coords);

    if (err != NoError) {
        ErrorBeep ("ACAPI_Selection_SetMarquee", err);
        return;
    }
}
```

---

---

## **📌 2. HIGHLIGHT FUNCTIONS**

---

### **🔹 ACAPI_UserInput_SetElementHighlight**
**Purpose**: Highlight elements in 2D and 3D windows with custom colors (Archicad 26+).

**Function Signature**:
```cpp
__APIEXPORT void ACAPI_UserInput_SetElementHighlight (
    const GS::HashTable<API_Guid, API_RGBAColor>& highlightedElems,
    const GS::Optional<bool>& wireframe3D = GS::NoValue,
    const GS::Optional<API_RGBAColor>& nonHighlightedElemsColor = GS::NoValue
);
```

**Parameters**:
- `highlightedElems`: [in] Hash table mapping element GUIDs to `API_RGBAColor`
- `wireframe3D`: [in] Optional - if true, non-highlighted elements in 3D appear in wireframe
- `nonHighlightedElemsColor`: [in] Optional - color and transparency for non-highlighted elements

**Remarks**: 
You can remove element highlight by calling `ACAPI_UserInput_ClearElementHighlight`.
After changing element highlights the model needs to be redrawn by calling `ACAPI_View_Redraw`.

---

#### **✅ Exact Usage Examples from SDK Reference:**

**Example 1** - Highlight with different colors:
```cpp
void HighlightSelectedElements(const GS::Array<API_Guid>& elemGuids) {
    GS::HashTable<API_Guid, API_RGBAColor> highlightMap;

    API_RGBAColor redColor = {1.0, 0.0, 0.0, 0.5};    // Semi-transparent red
    API_RGBAColor blueColor = {0.0, 0.0, 1.0, 0.7};  // Semi-transparent blue

    for (UInt32 i = 0; i < elemGuids.GetSize(); i++) {
        highlightMap.Add(elemGuids[i], (i % 2 == 0) ? redColor : blueColor);
    }

    bool wireframeBackground = true;
    API_RGBAColor dimColor = {0.7, 0.7, 0.7, 0.95}; // Gray, mostly opaque

    ACAPI_UserInput_SetElementHighlight(highlightMap, wireframeBackground, dimColor);
    ACAPI_View_Redraw(); // Refresh the view
}
```

**Example 2** - Highlight all meshes:
```cpp
void HighlightAllMeshes() {
    GS::Array<API_Guid> meshList;
    ACAPI_Element_GetElemList(API_MeshID, &meshList);

    if (meshList.GetSize() > 0) {
        GS::HashTable<API_Guid, API_RGBAColor> hlElems;
        API_RGBAColor hlColor = {0.0, 0.5, 0.75, 0.5};

        for (auto it = meshList.Enumerate(); it != nullptr; ++it) {
            hlElems.Add(*it, hlColor);
            hlColor.f_red += 0.1;
            if (hlColor.f_red > 1.0)
                hlColor.f_red = 0.0;
        }

        bool wireframe3D = false;
        API_RGBAColor nonHighlightedElemsColor = {0.7, 0.7, 0.7, 0.95};

        ACAPI_UserInput_SetElementHighlight(hlElems, wireframe3D, nonHighlightedElemsColor);
    }
}
```

---

---

### **🔹 ACAPI_UserInput_ClearElementHighlight**
**Purpose**: Remove all element highlights from 2D and 3D windows.

**Function Signature**:
```cpp
__APIEXPORT void ACAPI_UserInput_ClearElementHighlight ();
```

**Remarks**: This function removes element highlights set by `ACAPI_UserInput_SetElementHighlight`.
After changing element highlights the model needs to be redrawn by calling `ACAPI_View_Redraw`.

---

#### **✅ Exact Usage Examples from SDK Reference:**

**Example**:
```cpp
void ClearHighlights() {
    ACAPI_UserInput_ClearElementHighlight();
    ACAPI_View_Redraw();
}
```

---

---

## **📌 3. SELECTION NOTIFICATION**

---

### **🔹 ACAPI_Notification_CatchSelectionChange**
**Purpose**: Register/unregister callback for selection change notifications.

**Function Signature**:
```cpp
__APIEXPORT GSErrCode ACAPI_Notification_CatchSelectionChange (
    APISelectionChangeHandlerProc *handlerProc
);
```

**Callback Type**:
```cpp
typedef GSErrCode APISelectionChangeHandlerProc (const API_Neig* selElemNeig);
```

**Parameters**:
- `handlerProc`: [in] Callback function pointer (nullptr to unregister)

**Return Values**:
- `NoError` - The function completed successfully.

**Remarks**: 
This function is used to register/unregister an add-on which wants to monitor the changes in selection. You do not have to call `ACAPI_KeepInMemory` afterwards, as the API ensures that add-ons with installed notification handlers won't be unloaded. After registration your add-on's handlerProc you will be called when the selection changes.

**Callback Parameters**:
- `selElemNeig`: [in] This structure identifies the last selected element.

**Callback Return Values**:
- `NoError` - The function has completed with success.

---

#### **✅ Exact Usage Examples from SDK:**

**Example 1** - Register callback (Browser_Control/Src/Main.cpp:95-97):
```cpp
err = ACAPI_Notification_CatchSelectionChange (BrowserPalette::SelectionChangeHandler);
if (DBERROR (err != NoError))
    return err;
```

**Example 2** - Toggle monitoring (Notification_Manager/Src/Selection_Observer.cpp:50-57):
```cpp
void Do_SelectionMonitor (bool switchOn)
{
    if (switchOn)
        ACAPI_Notification_CatchSelectionChange (SelectionChangeHandlerProc);
    else
        ACAPI_Notification_CatchSelectionChange (nullptr);
    return;
}
```

**Example 3** - Callback implementation (Notification_Manager/Src/Selection_Observer.cpp:30-42):
```cpp
static GSErrCode SelectionChangeHandlerProc (const API_Neig* selElemNeig)
{
    if (selElemNeig->neigID != APINeig_None) {
        char msgStr[256];
        sprintf (msgStr, "Last selected element: NeigID %d; guid: %s, inIndex: %d",
                 selElemNeig->neigID, (const char *) APIGuid2GSGuid (selElemNeig->guid).ToUniString ().ToCStr (), selElemNeig->inIndex);
        ACAPI_WriteReport (msgStr, false);
    } else {
        ACAPI_WriteReport ("All elements deselected", false);
    }
    return NoError;
}
```

**Example 4** - Palette callback (Browser_Control/Src/BrowserPalette.cpp:215-220):
```cpp
GSErrCode BrowserPalette::SelectionChangeHandler (const API_Neig*)
{
    if (BrowserPalette::HasInstance ())
        BrowserPalette::GetInstance ().UpdateSelectedElementsOnHTML ();
    return NoError;
}
```

---

---

## **📌 4. SUPPORTING DATA STRUCTURES**

---

### **API_Neig Structure**
Used to identify selectable elements and their parts:

```cpp
struct API_Neig {
    API_NeigID          neigID;           // Element type identifier
    Int32               filler_1;         // Reserved
    API_Guid            guid;             // Element GUID
    Int32               inIndex;          // Sub-index inside element
    Int32               flags;            // Neig flags (Normal, HoleSel, Ghost, etc.)
    API_NeigElemPartID  elemPartType;     // Partial selection type (None, Edge, Vertex, Face, Subelem)
    UInt32              elemPartIndex;    // Index of element part
    short               subType;          // Subtype of element
    short               nodeType;         // Node type of element
    UInt32              supplUnId;        // Supplemental unique identifier used for certain neig types
};
```

**Neig Flags**:
- `API_NeigFlg_Normal` - Normal neig.
- `API_NeigFlg_HoleSel` - The neig is part of a hole.
- `API_NeigFlg_Extra3D` - The neig appears only in 3D.
- `API_NeigFlg_Ghost` - The neig is for an element coming from the ghost story.
- `API_NeigFlg_Surface` - The neig refers to a surface of an element (e.g. Morph face).

**Element Part Types**:
- `APINeigElemPart_None` - The whole element is selected.
- `APINeigElemPart_Edge` - An edge of the element is selected.
- `APINeigElemPart_Vertex` - A vertex of the element is selected.
- `APINeigElemPart_Face` - A face of the element is selected.
- `APINeigElemPart_Subelem` - A subelement is selected.

---

### **API_SelectionInfo Structure**
Used to get/set selection information:

```cpp
struct API_SelectionInfo {
    API_SelTypeID    typeID;         // Selection type (API_SelEmpty, API_SelElems, API_MarqueePoly, etc.)
    Int32            sel_nElem;       // Number of selected elements
    Int32            sel_nElemEdit;   // Number of editable selected elements
    Int32            filler_1[3];      // Reserved
    bool             filler_2[3];      // Reserved
    bool             multiStory;      // Marquee extends to all stories
    Int32            filler_3;         // Reserved
    API_Region       marquee;        // Marquee region parameters
};
```

**Selection Type IDs**:
- `API_SelEmpty` - No selection
- `API_SelElems` - Individual elements selected
- `API_MarqueePoly` - Polygon marquee selection
- `API_MarqueeHorBox` - Horizontal box marquee selection
- `API_MarqueeRotBox` - Rotated box marquee selection

---

### **API_RGBAColor Structure**
Used for highlight colors:

```cpp
struct API_RGBAColor {
    double  f_red;    // Red component (0.0 - 1.0)
    double  f_green;  // Green component (0.0 - 1.0)
    double  f_blue;   // Blue component (0.0 - 1.0)
    double  f_alpha;  // Alpha/opacity (0.0 - 1.0, 0=transparent, 1=opaque)
};
```

**Remarks**: Each component expressed in [0.0... 1.0] interval. The {0.0, 0.0, 0.0} components represents the black, {1.0, 1.0, 1.0} the white color.
The meaning of the f_alpha component: 0.0 means totally transparent, 1.0 means totally opaque color.

---

### **API_SelRelativePosID Enum**
Used with `ACAPI_Selection_Get` to filter elements relative to marquee:

```cpp
typedef enum {
    API_InsidePartially,     // Elements partially inside marquee
    API_InsideEntirely,      // Elements entirely inside marquee
    API_OutsidePartially,    // Elements partially outside marquee
    API_OutsideEntirely      // Elements entirely outside marquee
} API_SelRelativePosID;
```

---

---

---
---

## **📚 SUMMARY TABLE**

| **Function** | **Purpose** | **Header File** | **Available Since** |
|--------------|-------------|-----------------|-------------------|
| `ACAPI_Selection_Select` | Add/remove elements from selection | ACAPinc.h | All versions |
| `ACAPI_Selection_DeselectAll` | Clear all selections | ACAPinc.h | All versions |
| `ACAPI_Selection_Get` | Get current selection info | ACAPinc.h | All versions |
| `ACAPI_Selection_SetMarquee` | Set/modify marquee selection | ACAPinc.h | All versions |
| `ACAPI_UserInput_SetElementHighlight` | Highlight elements with colors | ACAPI_Interface.h | AC26 |
| `ACAPI_UserInput_ClearElementHighlight` | Remove all highlights | ACAPI_Interface.h | AC26 |
| `ACAPI_Notification_CatchSelectionChange` | Monitor selection changes | ACAPinc.h | All versions |

---

---
---

## **💡 KEY NOTES**

1. **Memory Management**: When using `ACAPI_Selection_SetMarquee` with `API_MarqueePoly` type, always release the coordinate handle with `BMKillHandle` when done.

2. **Error Handling**: Always check return values (GSErrCode) and handle errors appropriately. Note that `APIERR_NOSEL` is not a real error for `ACAPI_Selection_Get`.

3. **Highlight Requirement**: After calling highlight functions, call `ACAPI_View_Redraw()` to refresh the view.

4. **Partial Selection**: Use `ignorePartialSelection` parameter in `ACAPI_Selection_Get` to control whether element parts (edges, vertices, faces) are returned. This parameter is ignored in case of marquee based selection.

5. **Marquee Types**: `API_MarqueePoly`, `API_MarqueeHorBox`, `API_MarqueeRotBox` are valid for `ACAPI_Selection_SetMarquee`.

6. **Selection Types**: `API_SelEmpty`, `API_SelElems` indicate no selection or element selection respectively.

7. **Neig vs ElemType**: The `API_NeigID` differs from the `API_ElemTypeID`, because it refers to the selectable parts of the elements, not the elements themselves.

8. **Required Fields**: For `ACAPI_Selection_Select`, the neigID and the guid fields are required (inIndex and/or holeSel only where applicable).
