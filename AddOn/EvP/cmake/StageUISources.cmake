cmake_minimum_required (VERSION 3.16)

foreach (required UI_SOURCE_DIR UI_BUILD_DIR UI_MANIFEST)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message (FATAL_ERROR "${required} is required")
    endif ()
endforeach ()

file (STRINGS "${UI_MANIFEST}" sources)
set (previousManifest "${UI_BUILD_DIR}/.tapioca-staged-sources.txt")
set (previousSources "")
if (EXISTS "${previousManifest}")
    file (STRINGS "${previousManifest}" previousSources)
endif ()

# Only files recorded as staged source inputs can be removed. Never clean the
# workspace wholesale: it also owns node_modules and the last successful bundle.
foreach (source IN LISTS sources previousSources)
    string (REPLACE "\\" "/" normalizedSource "${source}")
    if (IS_ABSOLUTE "${source}" OR normalizedSource MATCHES "(^|/)[.][.](/|$)"
        OR NOT normalizedSource MATCHES "^(src|scripts|public)/|^(index[.]html|tsconfig[.]json|vite[.]config[.]ts|svelte[.]config[.]js)$")
        message (FATAL_ERROR "Unsafe UI source path: ${source}")
    endif ()
endforeach ()

foreach (source IN LISTS previousSources)
    if (NOT source IN_LIST sources)
        file (REMOVE "${UI_BUILD_DIR}/${source}")
    endif ()
endforeach ()
foreach (source IN LISTS sources)
    configure_file ("${UI_SOURCE_DIR}/${source}" "${UI_BUILD_DIR}/${source}" COPYONLY)
endforeach ()
configure_file ("${UI_MANIFEST}" "${previousManifest}" COPYONLY)
