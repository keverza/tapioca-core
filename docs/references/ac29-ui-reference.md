# Archicad 29 UI Reference

## Repository Disposition

- Authority: Archicad 29 API DevKit headers and examples under `AddOn/reference/archicad29-api-devkit/`.
- Conclusion: reference material only; verify each symbol against the local DevKit before implementation, while the current palette specification remains authoritative for shipped UI seams.

Based on the AC29 SDK documentation, here are the precise answers to your questions:

---

## **1. Create Disabled Input Field UI**

### **C++ (DG Library - Recommended)**
```cpp
// Create a disabled TextEdit field
DG::TextEdit textEdit (panel, rect, 0, DG::EditControl::Frame, DG::EditControl::Update, DG::EditControl::ReadOnly);

// Or create first, then disable
DG::TextEdit textEdit (panel, item);
textEdit.SetStatus (false);  // Disable
textEdit.Enable ();  // Enable
textEdit.Disable (); // Disable

// For other edit types (RealEdit, IntEdit, etc.)
DG::RealEdit realEdit (panel, rect, DG::EditControl::Frame, DG::EditControl::Absolute, DG::EditControl::Update, DG::EditControl::ReadOnly);
```

### **C API (Legacy)**
```cpp
DGDisableItem (dialId, item);  // Disable item
DGEnableItem (dialId, item);   // Enable item
DGSetItemEnable (dialId, item, 0); // Disable (0 = false)
DGSetItemEnable (dialId, item, 1); // Enable (1 = true)
```

**ReadOnlyType enum values:**
- `DG_ET_EDITABLE` = 0x0000 (editable)
- `DG_ET_READONLY` = 0x0100 (read-only/disabled)

---

## **2. Display and Render Table of Data in UI and Update**

### **Create ListView (Table)**
```cpp
// In resource (.grc) file:
ListView {
    ListViewID = 1001;
    x = 10; y = 10; width = 300; height = 200;
}

// In C++ code:
DG::ListView listView (panel, ListViewID);
```

### **Add/Update Table Data**
```cpp
// Add rows
listView.AppendItem ();  // Add at end
listView.InsertItem (0); // Insert at position 0

// Set cell text (row, column)
short rowIndex = 0;
listView.SetItemText (rowIndex, "Column 1 Data");
listView.SetItemDescription (rowIndex, "Tooltip text");

// Update existing row
listView.SetItemText (rowIndex, "Updated Data");

// Remove row
listView.DeleteItem (rowIndex);

// Get row count
short count = listView.GetItemCount ();

// Refresh display
listView.Update ();  // Or use DGUpdateDialog()
```

### **C API Functions**
```cpp
DGListViewInsertItem (dialId, item, listItem);  // Insert at position
DGListViewDeleteItem (dialId, item, listItem);  // Delete at position
DGListViewSetItemText (dialId, item, listItem, text);
DGListViewGetItemText (dialId, item, listItem);  // Returns GS::UniString
DGListViewSetItemDescription (dialId, item, listItem, description);
short count = DGListViewGetItemCount (dialId, item);

// Disable drawing (for performance with large tables)
DGListViewDisableDraw (dialId, item);
DGListViewEnableDraw (dialId, item);
```

### **Handle Selection**
```cpp
// Select item
listView.SelectItem (rowIndex);
listView.DeselectItem (rowIndex);

// Get selected items
short selCount = listView.GetSelCount ();
short selectedItems[100];
listView.GetSelItems (selectedItems, 100);

// Check if item selected
bool isSelected = listView.GetSelected (rowIndex);
```

### **Custom Drawing & Updates**
```cpp
// Subclass ListViewObserver to handle updates
class MyListViewObserver : public DG::ListViewObserver {
    void ListViewItemUpdate (const DG::ListViewUpdateEvent& ev, DG::Rect* imageRect) override {
        // Custom drawing logic
    }
};

// Attach observer
listView.Attach (*observer);
```

---

## **3. Read Property Value from Project and Prefill Input Field**

### **Read Property from Element**
```cpp
// Get single property by GUID
API_Guid elemGuid = /* element GUID */;
API_Guid propertyGuid = /* property GUID */;
API_Property property;

GSErrCode err = ACAPI_Element_GetPropertyValue (elemGuid, propertyGuid, property);
if (err == NoError && property.status == API_Property_HasValue) {
    // Property is valid and has a value
}

// Get multiple properties
GS::Array<API_PropertyDefinition> definitions;
err = ACAPI_Element_GetPropertyDefinitions (elemGuid, API_PropertyDefinitionFilter_All, definitions);
if (err == NoError) {
    GS::Array<API_Property> properties;
    err = ACAPI_Element_GetPropertyValues (elemGuid, definitions, properties);
    // Process properties array
}

// Get property by name from definitions
for (const auto& def : definitions) {
    if (def.name == "Your Property Name") {
        API_Property prop;
        ACAPI_Element_GetPropertyValue (elemGuid, def.guid, prop);
        break;
    }
}
```

### **Property Value Types**
```cpp
// API_PropertyValue contains:
switch (property.value.singleVariant.type) {
    case API_PropertyBooleanValueType:
        bool boolVal = property.value.singleVariant.boolValue;
        break;
    case API_PropertyIntegerValueType:
        Int64 intVal = property.value.singleVariant.intValue;
        break;
    case API_PropertyDoubleValueType:
        double doubleVal = property.value.singleVariant.doubleValue;
        break;
    case API_PropertyStringValueType:
        GS::UniString strVal = property.value.singleVariant.uniStringValue;
        break;
    case API_PropertyLengthValueType:
        double lengthVal = property.value.singleVariant.doubleValue; // In project units
        break;
    // Handle list variants for multi-value properties
    case API_PropertyListCollectionType:
        // property.value.listVariant.variants contains array
        break;
}
```

### **Prefill Input Field with Property Value**
```cpp
// Read property and set to TextEdit
API_Property property;
if (ACAPI_Element_GetPropertyValue (elemGuid, propertyGuid, property) == NoError &&
    property.status == API_Property_HasValue) {

    // For string property
    if (property.value.singleVariant.type == API_PropertyStringValueType) {
        GS::UniString value = property.value.singleVariant.uniStringValue;
        textEdit.SetText (value);
    }

    // For numeric property
    if (property.value.singleVariant.type == API_PropertyDoubleValueType) {
        double value = property.value.singleVariant.doubleValue;
        realEdit.SetValue (value);
        intEdit.SetValue (static_cast<Int32>(value));
    }

    // For length property
    if (property.value.singleVariant.type == API_PropertyLengthValueType) {
        double value = property.value.singleVariant.doubleValue;
        lengthEdit.SetValue (value);
    }
}

// For read-only prefilled field
DG::TextEdit readOnlyEdit (panel, rect, 0, DG::EditControl::Frame, DG::EditControl::NoUpdate, DG::EditControl::ReadOnly);
readOnlyEdit.SetText (prefilledValue);
```

### **Convert Property to String for Display**
```cpp
// Get display string from property value
GS::UniString displayString;
err = ACAPI_Property_GetPropertyValueString (property, &displayString);
if (err == NoError) {
    textEdit.SetText (displayString);
}
```

### **Get Project-Wide Properties**
```cpp
// For project info properties (without element GUID)
API_PropertyDefinition projectPropertyDef;
API_Guid projectPropertyGuid = /* known project property GUID */;
API_Property projectProperty;

err = ACAPI_Property_GetPropertyDefinition (projectPropertyGuid, projectPropertyDef);
err = ACAPI_Element_GetPropertyValueOfDefaultElem (API_ProjectInfoID, projectPropertyGuid, projectProperty);
```

---

## **Key References from SDK Documentation**

- **DGEditControl.hpp**: Lines 408-411 - `ReadOnlyType` enum for disabled state
- **DGItem.hpp**: Line 602 - `SetStatus(bool enable)` method
- **DG.h**: Lines 1091-1097 - `DGDisableItem()`, `DGEnableItem()`, `DGSetItemEnable()`
- **DGListView.hpp**: Lines 431-449 - Table row management methods
- **APIdefs_Properties.h**: Lines 560-593 - `API_Property` struct definition
- **ACAPinc.h**: Lines 4866-4954 - Property reading functions


---

## **4. Resize ListView (Table) Vertically by Dragging**

**ListView itself does NOT support direct vertical dragging.** To create a resizable table, you need to combine **ListView + Splitter control** below it. The user drags the splitter, and you programmatically resize the ListView height.

---

### **Method A: Using Splitter Control (Recommended)**

#### **Step 1: Create Layout in Resource File (.grc)**
```grc
// Dialog resource
Dialog {
    DialogID = 1000;
    width = 400; height = 500;

    ListView {
        ListViewID = 1001;
        x = 10; y = 10; width = 380; height = 200;
    }

    Splitter {
        SplitterID = 1002;
        x = 10; y = 220; width = 380; height = 8;
        type = Horizontal;
    }
}
```

#### **Step 2: Create Splitter Observer**
```cpp
class ListViewResizeObserver : public DG::SplitterObserver {
private:
    DG::ListView& listView;

public:
    ListViewResizeObserver (DG::ListView& lv) : listView (lv) {}

    void SplitterDragged (const DG::SplitterDragEvent& ev) override {
        // Get new splitter position
        short newY = ev.GetPosition ();

        // Calculate new ListView height (from top to splitter position - gap)
        short newHeight = newY - 10; // 10 = top margin

        // Resize ListView
        DG::Rect currentRect = listView.GetRect ();
        listView.SetHeight (newHeight);

        // Update splitter position
        DG::Rect splitterRect = ev.GetSource ()->GetRect ();
        splitterRect.top = newY;
        ev.GetSource ()->SetRect (splitterRect);
    }

    void SplitterDragStarted (const DG::SplitterDragEvent& ev) override {
        // Optional: Start tracking
    }

    void SplitterDragExited (const DG::SplitterDragEvent& ev) override {
        // Optional: Cleanup
    }
};
```

#### **Step 3: Initialize in Dialog Code**
```cpp
// Create controls
DG::ListView listView (panel, 1001);
DG::Splitter splitter (panel, 1002);

// Create and attach observer
ListViewResizeObserver observer (listView);
splitter.Attach (observer);

// Enable dragging on splitter
splitter.EnableDrag ();  // Or: DGSplitterEnableDrag (dialId, 1002);
```

#### **Step 4: Update ListView Size Programmatically**
```cpp
// Direct resize
DG::Rect rect = listView.GetRect ();
rect.height = newHeight;
listView.SetRect (rect);

// Or using individual setters
listView.SetHeight (newHeight);
listView.SetWidth (newWidth);
listView.SetPosition (x, y);
```

---

### **Method B: Using C API**

```cpp
// Enable splitter dragging
DGSplitterEnableDrag (dialId, SplitterID);

// Disable splitter dragging
DGSplitterDisableDrag (dialId, SplitterID);

// Check if dragging is enabled
bool isEnabled = DGSplitterIsDragEnabled (dialId, SplitterID);

// Set drag status
DGSplitterSetDragStatus (dialId, SplitterID, true);

// Resize ListView
DGResizeItem (dialId, ListViewID, 0, newHeight);  // Horizontal resize = 0, vertical resize = newHeight
DGMoveItem (dialId, ListViewID, 0, 0);  // Optional: move position
```

---

### **Splitter Types & Orientation**

| Type | Description |
|------|-------------|
| `DG::Splitter::Horizontal` or `DG_SPLT_HORIZONTAL` (0) | Horizontal splitter (for vertical resizing) |
| `DG::Splitter::Vertical` or `DG_SPLT_VERTICAL` (1) | Vertical splitter (for horizontal resizing) |
| `DG::Splitter::Normal` (0) | Normal splitter appearance |
| `DG::Splitter::Transparent` or `DG_SPLT_TRANSPARENT` (2) | Transparent splitter |

**For vertical table resizing:** Use **Horizontal splitter** placed **below** the ListView.

---

### **Complete Example: Vertically Resizable ListView**

```cpp
// Header
class ResizableListViewDialog : public DG::Dialog {
private:
    DG::ListView listView;
    DG::Splitter splitter;
    ListViewResizeObserver observer;
public:
    ResizableListViewDialog () :
        DG::Dialog (ACAPI_GetOwnResModule (), DialogID),
        listView (*this, 1001),
        splitter (*this, 1002),
        observer (listView)
    {
        splitter.Attach (observer);
        splitter.EnableDrag ();

        // Populate initial data
        for (int i = 0; i < 20; i++) {
            listView.AppendItem ();
            listView.SetItemText (i, GS::UniString::Printf ("Item %d", i));
        }
    }
};

// Observer implementation
void ListViewResizeObserver::SplitterDragged (const DG::SplitterDragEvent& ev) {
    short splitterY = ev.GetPosition ();
    short minHeight = 50;  // Minimum ListView height
    short maxHeight = 400; // Maximum ListView height

    // Constrain height
    short newHeight = std::max (minHeight, std::min (maxHeight, splitterY - 10));

    // Resize ListView
    listView.SetHeight (newHeight);

    // Update splitter Y position (splitter height is 8)
    DG::Rect sRect = splitter.GetRect ();
    sRect.top = 10 + newHeight;
    splitter.SetRect (sRect);
}
```

---

### **Key SDK References**

- **DGSplitter.hpp**: Lines 118-163 - `Splitter` class with `EnableDrag()`, `DisableDrag()`, `SetDragStatus()`
- **DGItem.hpp**: Lines 600-603 - `Enable()`, `Disable()`, `SetStatus(bool)`
- **DG.h**: Lines 2029-2035 - `DGSplitterEnableDrag()`, `DGSplitterDisableDrag()`, `DGSplitterSetDragStatus()`, `DGSplitterIsDragEnabled()`
- **DGDefs.h**: Lines 703-705 - Splitter orientation constants
