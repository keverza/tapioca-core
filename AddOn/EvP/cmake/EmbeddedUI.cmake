# Build resources without reinstalling the source tree's development dependencies.
# Windows cannot unlink a native .node module loaded by a watcher/editor. Each
# CMake build tree therefore owns its own npm installation and staged sources.
function (TapiocaAddEmbeddedUI name sourceFolder)
    set (buildFolder "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedUI/${name}")
    set (output "${buildFolder}/dist/index.html")
    set (installStamp "${buildFolder}/node_modules/.tapioca-install.stamp")
    set (sourceManifest "${buildFolder}/sources.txt")
    # CMAKE_CURRENT_FUNCTION_LIST_DIR requires CMake 3.17. The caller supplies
    # the helper directory instead so the add-on's 3.16 minimum remains valid.
    set (stageScript "${TAPIOCA_EMBEDDED_UI_CMAKE_DIR}/StageUISources.cmake")
    file (MAKE_DIRECTORY "${buildFolder}")
    foreach (manifest package.json package-lock.json)
        configure_file ("${sourceFolder}/${manifest}" "${buildFolder}/${manifest}" COPYONLY)
    endforeach ()

    set (manifestContents "")
    foreach (source IN LISTS ARGN)
        file (RELATIVE_PATH relativeSource "${sourceFolder}" "${source}")
        string (APPEND manifestContents "${relativeSource}\n")
    endforeach ()
    # Only changes when the file list changes: removals must rebuild the bundle,
    # while an unrelated native reconfigure must not reinstall npm dependencies.
    file (GENERATE OUTPUT "${sourceManifest}" CONTENT "${manifestContents}")

    add_custom_command (
        OUTPUT "${installStamp}"
        DEPENDS "${buildFolder}/package.json" "${buildFolder}/package-lock.json"
        WORKING_DIRECTORY "${buildFolder}"
        COMMENT "EvP: installing isolated ${name} dependencies"
        COMMAND "${NPM_EXECUTABLE}" ci
        COMMAND "${CMAKE_COMMAND}" -E touch "${installStamp}"
        VERBATIM)

    add_custom_command (
        OUTPUT "${output}"
        DEPENDS ${ARGN} "${sourceManifest}" "${stageScript}" "${installStamp}"
        WORKING_DIRECTORY "${buildFolder}"
        COMMENT "EvP: building the self-contained Tapioca ${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-DUI_SOURCE_DIR=${sourceFolder}"
            "-DUI_BUILD_DIR=${buildFolder}"
            "-DUI_MANIFEST=${sourceManifest}"
            -P "${stageScript}"
        COMMAND "${NPM_EXECUTABLE}" run typecheck
        COMMAND "${NPM_EXECUTABLE}" run build
        VERBATIM)
    add_custom_target (EvP${name} ALL DEPENDS "${output}")
    set_target_properties (EvP${name} PROPERTIES FOLDER Resources)
    set (${name}BuildFolder "${buildFolder}" PARENT_SCOPE)
    set (${name}Output "${output}" PARENT_SCOPE)
endfunction ()
