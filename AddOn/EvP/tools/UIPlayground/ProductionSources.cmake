# Explicit production translation units, verified against the offline suite at configure time.
set (panelSources
    ArchViz/HudMassingStats.cpp ArchViz/MassingUsage.cpp ArchViz/MassingAreaCalculations.cpp
    ArchViz/HudFloorProgramme.cpp ArchViz/FloorProgramme.cpp
    ArchViz/HudShell.cpp ArchViz/HudShellStyle.cpp ArchViz/HudShellTabs.cpp
    ArchViz/HudShellDock.cpp ArchViz/HudShellNavigation.cpp ArchViz/OverlayHitMap.cpp
    ArchViz/OverlayLayers.cpp ArchViz/PlanOverlayContent.cpp ArchViz/GraphicsSettings.cpp ArchViz/GraphicsSettingsUi.cpp
    ArchViz/TapiocaUI/Foundation/ScopedStyle.cpp ArchViz/TapiocaUI/Foundation/Units.cpp
    ArchViz/TapiocaUI/Foundation/Icons.cpp ArchViz/TapiocaUI/Foundation/Iconoir.gen.cpp
    ArchViz/TapiocaUI/Demo/WidgetGallery.cpp
    Metadata/TapiocaSchema.cpp Metadata/TapiocaMetadata.cpp)
file (READ "${EVP_ROOT}/tests/cpp/CMakeLists.txt" offlineSources)
foreach (source IN LISTS panelSources)
    if (NOT EXISTS "${ADDON_DIR}/${source}")
        message (FATAL_ERROR "Missing production source: ${source}")
    endif ()
    string (FIND "${offlineSources}" "/Sources/AddOn/${source}\"" found)
    if (found EQUAL -1)
        message (FATAL_ERROR "Playground source is not covered by tests/cpp: ${source}")
    endif ()
    list (APPEND PLAYGROUND_PRODUCTION_SOURCES "${ADDON_DIR}/${source}")
endforeach ()
file (GLOB_RECURSE uiSources CONFIGURE_DEPENDS "${ADDON_DIR}/ArchViz/TapiocaUI/*.cpp")
foreach (source IN LISTS uiSources)
    if (NOT source IN_LIST PLAYGROUND_PRODUCTION_SOURCES)
        message (FATAL_ERROR "New TapiocaUI source missing from playground manifest: ${source}")
    endif ()
endforeach ()
