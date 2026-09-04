cmake_minimum_required (VERSION 3.16)

# ============================================================================
# EvPGeomTests â€” L2 offline unit tests for Sources/AddOn/Geometry/
# (see docs/guides/testing.md Â§2)
#
# STANDALONE ON PURPOSE. This is its own CMake project, NOT a target inside the
# add-on's CMakeLists, because:
#   * the add-on's CMakeLists hard-fails without the Archicad DevKit, and the
#     whole point of L2 is that it needs no DevKit, no Archicad, no Python;
#   * that makes this buildable on Linux/WSL and in hosted CI, where the .apx
#     cannot be built at all (docs/guides/testing.md Â§8.7).
#
# It compiles the REAL engine sources â€” never a copy â€” so a test can only pass
# against the code that ships.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# Sanitizers (docs/guides/testing.md Â§5): -DEVP_SANITIZE=ON.
# ============================================================================

# C as well as CXX: quickjs-ng is a C library. That was previously enabled as a
# SIDE EFFECT of the vendored font libraries adding their own C subdirectories,
# so making the text stack optional turned it off and quickjs lost its linker
# language. The suite states its own languages instead of inheriting them from
# a third-party subdirectory it may or may not include.
project (EvPGeomTests C CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (EVP_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set (GEOM_DIR   "${EVP_ROOT}/Sources/AddOn/Geometry")
set (REF_DIR    "${EVP_ROOT}/../reference")
set (GTEST_DIR  "${REF_DIR}/googletest")
set (NANORT_INC "${REF_DIR}/nanort-release")
set (GLM_INC "${REF_DIR}/glm-master")
set (CLIPPER2_DIR "${REF_DIR}/Clipper2-main/CPP")
set (QUICKJS_DIR "${REF_DIR}/quickjs-ng")
# The vendored text stack (FreeType + HarfBuzz + msdfgen + msdf-atlas-gen) is
# BEHIND A SWITCH, DEFAULT OFF, while the scene-text feature it serves is
# half-finished.
#
# It is off by default because of what it costs everyone who is NOT working on
# text: four third-party libraries configure and compile on every fresh build
# tree, and an in-progress change anywhere in that stack stops the whole offline
# suite - including the node-graph tests, which have nothing to do with text.
# A gate that a half-finished neighbouring feature can turn red is a gate people
# start ignoring.
#
# âš ï¸ TURNING IT OFF DROPS REAL COVERAGE, so it says so out loud rather than
# quietly building fewer tests. Turn it back on with -DEVP_TEXT_RENDERING=ON,
# which is what anyone picking the feature back up must do.
option (EVP_TEXT_RENDERING "Build the vendored text-rendering stack and its tests" OFF)
if (EVP_TEXT_RENDERING)
    include ("${EVP_ROOT}/cmake/TextRendering.cmake")
else ()
    message (STATUS "EVP_TEXT_RENDERING=OFF: skipping FreeType/HarfBuzz/msdfgen and the SceneText* tests. Re-enable with -DEVP_TEXT_RENDERING=ON.")
    set (TAPIOCA_TEXT_LIBS "")
    set (TAPIOCA_TEXT_FONT "")
endif ()
# No msgpack include on purpose: the serializer hand-rolls its writer, and
# test_serializer.cpp hand-rolls an INDEPENDENT reader from the msgpack spec.
# Round-tripping through the same library the writer used would prove less.

option (EVP_SANITIZE "Build the tests with ASan/UBSan" OFF)

# ---- the engines under test ------------------------------------------------
# GeometryExtractor.cpp is deliberately ABSENT: it is the only file in Geometry/
# that pulls the DevKit (Model.hpp, ACAPinc.h). It stays thin glue and is covered
# at L5, not here. Adding it would break the no-DevKit promise above.
set (EVP_GEOM_SOURCES
    "${GEOM_DIR}/SliceEngine.cpp"
    "${GEOM_DIR}/ClashEngine.cpp"
    "${GEOM_DIR}/QueryEngine.cpp"
    "${GEOM_DIR}/MeshSerializer.cpp"
    "${GEOM_DIR}/RenderEngine.cpp"
    "${GEOM_DIR}/SpatialQueries.cpp"
    "${GEOM_DIR}/VertexWeld.cpp"
    "${GEOM_DIR}/GeometryEngine.cpp"
    # Primitives.cpp builds the solids the node catalog offers. Pure maths with
    # no ACAPI, and every way of getting it wrong is a PICTURE - a rounded box, a
    # black pole, a solid half-missing under back-face culling - so it belongs
    # under an offline gate rather than in front of someone in Archicad.
    "${GEOM_DIR}/Primitives.cpp"
    # Curves.cpp and Transforms.cpp are the other two halves of the modelling
    # catalog, and both fail as PICTURES rather than as errors - an arc that
    # collapses because its plane normal was crossed with a parallel axis, a
    # mirrored mesh that is inside-out, a normal transformed as a direction under
    # a non-uniform scale. None of those logs anything.
    "${GEOM_DIR}/Curves.cpp"
    "${GEOM_DIR}/Transforms.cpp")
list (APPEND EVP_GEOM_SOURCES "${GEOM_DIR}/WireframeEdges.cpp")

# Not an engine, but it makes the same promise: Palette/CommandFilter.cpp is pure
# string work with no DevKit and no DG, and a search ranking is exactly what cannot
# be judged by eye in Archicad â€” "did my change make the right command come first"
# is a question only a test answers.
# Palette/ParamVisibility.cpp joins it on the same terms (F3): show_when decides
# which parameter rows exist, and a row that wrongly vanishes reports nothing at
# all in Archicad â€” it just looks like a command with fewer settings.
set (EVP_PALETTE_SOURCES
    "${EVP_ROOT}/Sources/AddOn/Palette/AutomaticPreviewState.cpp"
    "${EVP_ROOT}/Sources/AddOn/Palette/CommandFilter.cpp"
    "${EVP_ROOT}/Sources/AddOn/Palette/ParamLayout.cpp"
    "${EVP_ROOT}/Sources/AddOn/Palette/ParamVisibility.cpp"
    "${EVP_ROOT}/Sources/AddOn/Palette/PreviewPlanCamera.cpp"
    "${EVP_ROOT}/Sources/AddOn/Palette/PreviewPanelState.cpp")

set (EVP_COMMAND_SOURCES
    "${EVP_ROOT}/Sources/AddOn/Python/ForcedParamMerge.cpp")

# Grasshopper/HostState.cpp is the Grasshopper worker's lifecycle rules with
# nothing else in them -- no DevKit, no Win32, no CLR, so it qualifies here on
# the same terms as everything above. It earns its place for the ArchViz reason
# turned up one notch: every way of getting these transitions wrong produces a
# SECOND worker process, a host that claims to be running after a failed start,
# or a worker message served by an .apx that is already unloading, and none of
# the three reports itself. A live run cannot exercise them either -- there is no
# way to click a menu item twice fast enough to test the concurrent-start rule.
#
# Grasshopper/GhProtocol.cpp is the Archicad<->worker wire format, and it belongs
# here for a sharper version of the same argument. Framing is where a transport
# fails SILENTLY: a length read as a type, a payload cap checked after the
# allocation, a version accepted because nobody compared it. None of those
# produces an error at the boundary -- they produce a worker that reads one
# message as another, and a live Archicad tells you only that "Grasshopper did
# not work".
# WARNING: KEEP BOTH DevKit-FREE AND Win32-FREE. The moment either needs
# ACAPinc.h or <windows.h> it must leave this list, and the only offline gate on
# the worker's lifecycle and transport goes with it.
#
# Grasshopper/GhPreviewProtocol.cpp is the PREVIEW half of that wire, and it is
# the sharpest case in this whole file. On every other path in here the .apx is
# the producer; on this one the producer is a separate process running
# third-party Grasshopper components, and its declared counts, offsets and
# lengths end up indexing into memory inside Archicad.exe. An unvalidated
# segment offset is not a message read as another message -- it is an
# out-of-bounds read in the host, reachable from any definition a user opens.
# A malformed producer cannot be exercised live either: the producer Tapioca
# ships is the one that behaves.
set (EVP_GRASSHOPPER_SOURCES
    "${EVP_ROOT}/Sources/AddOn/Grasshopper/GhProtocol.cpp"
    "${EVP_ROOT}/Sources/AddOn/Grasshopper/GhPreviewProtocol.cpp"
    "${EVP_ROOT}/Sources/AddOn/Grasshopper/GhUndoBudget.cpp"
    "${EVP_ROOT}/Sources/AddOn/Grasshopper/HostState.cpp")

set (EVP_DYNAMO_SOURCES
    "${EVP_ROOT}/Sources/AddOn/Dynamo/ApiPipeProtocol.cpp")

# SunStudy is the renderer-agnostic sun/shadow analysis core. It deliberately
# includes no Diligent header and calls no ACAPI, which is what lets the Diligent
# viewport, the WebGL study page and a headless run share one engine -- and what
# puts the whole of it under this offline gate rather than behind a live Archicad
# run. SunStudyRaster.cpp is the shadow camera's arithmetic, transcribed from
# private/Commands/SunStudy/sunraster.py; test_sunstudyraster.cpp pins the two
# together.
set (EVP_SUNSTUDY_SOURCES
    "${EVP_ROOT}/Sources/AddOn/SunStudy/SunStudyRaster.cpp"
    "${EVP_ROOT}/Sources/AddOn/SunStudy/CpuTraversal.cpp")

# NodeGraph is the owned graph document and evaluator with no host dependencies.
# WARNING: KEEP IT DevKit-FREE, DG-FREE AND Win32-FREE. If that changes, these
# real production sources can no longer be covered by the offline gate.
#
# ONE file in that directory is deliberately excluded and must stay excluded:
# ArchicadHostImpl.cpp is the ACAPI implementation of IArchicadHost. Everything
# else - including the Archicad NODES themselves - is written against that
# interface and is therefore listed below and covered here with a stub host.
# Adding ArchicadHostImpl.cpp to this list will not link; adding anything that
# includes ACAPinc.h has the same effect and means the boundary has been broken.
set (EVP_NODEGRAPH_SOURCES
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/AnyTree.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/AtomicValue.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/DataPath.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/Metadata.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/TreeIteration.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/TreeSerialization.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Data/ValueTypeAssistant.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ArchicadHost.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ArchicadNodes.cpp"
    # The element type table. ElementReaderImpl.cpp - the ACAPI half that
    # fills it in - is deliberately ABSENT for the same reason
    # ArchicadHostImpl.cpp is: it includes the DevKit. The table itself is
    # pure, so what an element type IS and how a selection groups by it are
    # covered offline.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ElementClassification.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/BuiltinNodes.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/EvaluationPlan.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Evaluator.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/FaultBarrier.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Graph.cpp"
    # GeometryNodes is the geometry and transform FAMILY, split out of
    # BuiltinNodes the way ArchicadNodes already is: a family is the unit of
    # ownership, and one file carrying inputs, inspection, flow AND every solid,
    # curve and transform had grown past the point where anyone could find the
    # one they came for.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GeometryNodes.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ParameterDescriptors.cpp"
    # NodeInputs is the one place that states "a wired input wins over the
    # typed-in one", so no family can implement that precedence backwards.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/NodeInputs.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/NodeLifting.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphAlgorithms.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphEdit.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphReports.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphRuntimeState.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphSerializer.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/GraphStore.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Json.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/NodeExecution.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/NodeRegistry.cpp"
    # PreviewProjection turns a graph's RESULTS into preview primitives. It is
    # here, in the node-graph suite rather than the preview one, because what it
    # has to get right is a graph question: which nodes preview, whether a
    # disabled one still does, and what a list of mixed values becomes.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/PreviewProjection.cpp"
    # The store it publishes into is Preview/GraphPreviewStore.cpp, listed with
    # the other preview sources below - one binary, so listing it twice would be
    # deduplicated rather than an error, which is exactly why it is worth not
    # doing.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ProjectGenerations.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ReferenceResolver.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/RunEvents.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/RunHistory.cpp"
    # The script node family. All of it is admitted EXCEPT ScriptWatcherWin32.cpp,
    # which includes <windows.h> - the same line ArchicadHostImpl.cpp sits on the
    # far side of. What that costs is the watcher's debounce; what it preserves is
    # that everything the watcher TRIGGERS - reading the file, parsing the header,
    # reshaping the node, dropping the edges that no longer fit - is covered here
    # with no Archicad and no APX rebuild.
    #
    # âš ï¸ ScriptRuntimeJs.cpp IS IN THIS LIST, and that is the point of embedding a
    # JS engine rather than borrowing the WebView's: a script node's evaluation,
    # its value marshalling and its time budget are all exercised offline. The
    # moment this file needs anything but quickjs and the standard library, it
    # must leave - and the offline gate for the riskiest code in the catalog,
    # which is code the USER wrote, goes with it.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptManifest.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptNodes.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptReload.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptRuntime.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptRuntimeJs.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptSource.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptValueJson.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ScriptWorkspace.cpp"
    # tree.* - the node family whose job is the shape of the data rather than
    # its content, so it opts out of NodeLifting via NodeType::treeBody. See
    # TreeNodes.hpp.
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/TreeNodes.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ListNodes.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/InputGathering.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/Value.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/ValueText.cpp"
    "${EVP_ROOT}/Sources/AddOn/NodeGraph/WorkerPool.cpp")

# ArchViz/MeshGroups.cpp qualifies on the same terms: no DevKit, no bgfx, no bx,
# nothing but <vector> and <algorithm>. It earns its place because getting it
# wrong does not FAIL -- every surface of an element renders in the first
# material's colour, which reads as a styling choice (plan Â§6.3, and the three.js
# side hit it once). That is invisible from inside Archicad unless you already
# know what the model should look like.
# âš ï¸ KEEP IT DevKit-FREE. The moment MeshGroups.cpp includes <bgfx/bgfx.h> it
# must leave this list, and the offline gate for Phase 6's riskiest algorithm
# goes with it.
# ArchViz/MaterialTable.cpp joins them on the same terms and for the same
# reason: a wrong material lookup does not fail, it renders the building in
# another surface's colours, and from inside Archicad that is indistinguishable
# from a design decision.
# ArchViz/DiligentClearCheck.cpp is here for the Diligent migration: it is the
# verdict half of the PLAT-RE22 clear A/B, deliberately kept free of D3D11 and
# Diligent so the sRGB/channel-order reasoning can be checked without a GPU.
# Getting it wrong reports a rendering fault that is not there (or hides one
# that is), and it took three live Archicad runs to notice that the last time.
# âš ï¸ KEEP IT <d3d11.h>-FREE â€” the DXGI format numbers are spelled out on purpose.
# The three scene-text engines and their tests, listed once and appended only
# when the stack above is built. They are the only sources here that need
# FreeType/HarfBuzz headers.
set (EVP_SCENE_TEXT_SOURCES
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SceneTextAtlas.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SceneTextLayout.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SceneTextLayoutCache.cpp")
set (EVP_SCENE_TEXT_TESTS
    test_scenetextatlas.cpp
    test_scenetextlayout.cpp
    test_scenetextlayoutcache.cpp)
if (NOT EVP_TEXT_RENDERING)
    set (EVP_SCENE_TEXT_SOURCES "")
    set (EVP_SCENE_TEXT_TESTS "")
endif ()

set (EVP_ARCHVIZ_SOURCES
    "${EVP_ROOT}/Sources/AddOn/ArchViz/MeshGroups.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/MaterialTable.cpp"
    # ArchViz/SurfaceClassifier.cpp decides what a surface IS from its numeric
    # channels alone. It is here for the strongest version of the ArchViz
    # argument: every wrong threshold renders a complete, correct building in
    # which one material is confidently the wrong substance, and the only way to
    # catch that is to run the real 219-surface template through it offline --
    # which needs no DevKit, because the classifier deliberately takes a plain
    # SurfaceMaterial and reads no name.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SurfaceClassifier.cpp"
    # ArchViz/BuildingMaterialSignal.cpp answers what a surface IS from the
    # building material behind the element, which is the only place that
    # information exists -- an Archicad surface describes appearance, not
    # substance. It is here because its whole design rests on two measured
    # facts (a standard priority scale, non-transferable fill guids) that can
    # only be checked by running it over two real projects offline.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/BuildingMaterialSignal.cpp"
    # ArchViz/SubstanceJoin.cpp is what finally gives that classifier a caller:
    # it folds the many-to-many body->surface join into one verdict per surface.
    # âš ï¸ IT IS HERE FOR THE SHARPEST VERSION OF THE ARCHVIZ REASON. Every way of
    # getting the join wrong renders a complete, plausible building in the wrong
    # materials -- no crash, no warning, and nothing on screen that says the
    # timber studs are being drawn as concrete. Pure by construction: no ACAPI,
    # no ModelerAPI, no Diligent, so the vote can be exercised offline against
    # exactly the ambiguous cases a real project produces.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SubstanceJoin.cpp"
    # ArchViz/AutoExposure.cpp is RE51.B9's exposure estimate and its white
    # balance. It is here because BOTH halves fail silently and plausibly: an
    # exposure that is wrong by a stop reads as a styling choice, and a white
    # balance applied in the wrong SENSE warms the image when asked to cool it
    # and looks entirely deliberate. Pure by construction -- which is the whole
    # reason the Kelvin arithmetic lives in C++ rather than in HLSL.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/AutoExposure.cpp"
    # ArchViz/ColourSpace.cpp is the sRGB transfer function. It is here because
    # a colour-space error is invisible in isolation and poisons every later
    # calibration: the RAL measurements that prove Archicad's colours are
    # display-referred are arithmetic, so they can be pinned exactly, offline.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/ColourSpace.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PointCloudPly.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PointCloudHierarchy.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PointCloudLod.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SceneCmdQueue.cpp"
    "${EVP_ROOT}/Sources/AddOn/ArchViz/DiligentClearCheck.cpp"
    # ArchViz/MatrixMath.cpp is the camera's two bx calls, rewritten bx-free for
    # the Diligent port. It belongs here more than almost anything else in
    # ArchViz: a left-handed view matrix over Archicad's right-handed world
    # renders the building MIRRORED, and a debug cube survives that looking
    # perfectly fine. It shipped once and was found by measuring which way pan
    # went, not by looking.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/MatrixMath.cpp"
    # ArchViz/PickVote.cpp is the rule that decides WHICH element a click selects.
    # It has been wrong three times (PLAT-RE44 and the accuracy reports after it),
    # every time found by a human clicking on a building and reporting that the
    # wrong thing lit up -- a build, a sync and a restart per correction. The rule
    # is arithmetic over 64 integers, so it belongs here.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PickVote.cpp"
    # ArchViz/DebugCubeMesh.cpp is the instrument the renderer is calibrated
    # with. A reversed winding here and a mirrored view matrix produce the SAME
    # picture, so the cube has to be known-good before looking at it proves
    # anything about the renderer.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/DebugCubeMesh.cpp"
    # ArchViz/AxisGnomonMesh.cpp is the instrument that answers "is the image
    # mirrored". Each arrow is one local geometry through a different axis
    # frame, and a frame whose (u x v) is not d is a REFLECTION -- the arrow
    # still points the right way while every triangle winds inside out.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/AxisGnomonMesh.cpp"
    # ArchViz/SunShadowMath.cpp decides where the sun's shadow camera goes, and
    # every way of getting it wrong produces a PICTURE rather than an error: a
    # frustum too small clips the shadow at a straight edge, too large blurs it,
    # inverted in depth shadows everything except what should be shadowed. The
    # test transforms the AABB's own corners through the matrix it returns and
    # checks they land inside the clip volume -- which is the property, not a
    # restatement of the arithmetic.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/SunShadowMath.cpp"
    # ArchViz/EnvironmentLighting.cpp reduces an HDR sky to nine SH
    # coefficients. It is here for the sharpest version of the usual ArchViz
    # reason: a stray factor of pi lights the whole building three times too
    # brightly, which reads as an EXPOSURE problem and gets tuned out
    # elsewhere in the shader -- after which the error is permanent and
    # invisible. A constant sky has an exactly known answer; nothing in
    # Archicad does.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/EnvironmentLighting.cpp"
    # ArchViz/PlanCameraMath.cpp turns a floor-plan window's measured
    # pixel-to-model mapping into a top-down orthographic camera. It belongs
    # here for the usual ArchViz reason -- every way of getting it wrong draws a
    # PICTURE -- and for one more: the two mappings it must REFUSE (sheared, and
    # mirrored) cannot be produced in Archicad on demand, so the only place they
    # can be exercised at all is a test that builds them by hand.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PlanCameraMath.cpp"
    # ArchViz/PlanAnchorRibbon.cpp turns a wall's 2D plan outline into the
    # triangles that draw it over the floor plan (PLAT-RE65). Same reason again
    # -- a wrong ribbon is a picture, not an error -- plus the arc half, which
    # carries a convention the repo CONTRADICTS ITSELF ABOUT (CommandUtils says
    # a positive arcAngle bulges right, polygon_area treats it as bulging left).
    # A test can at least pin down what this file does, so the live run only has
    # to settle which way Archicad means.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/PlanAnchorRibbon.cpp"
    # ArchViz/StorySliceUnion.cpp is the C++ port of the Python overlay's
    # sliceunion.py, and it is here for a reason the other entries do not have:
    # there are now TWO implementations of one boolean union -- this one draws in
    # the viewer, the Python one draws in Archicad -- and they are supposed to
    # produce the same outline for the same storey. test_storysliceunion.cpp is
    # deliberately the same cases as test_sliceunion.py so that "the same" is
    # checked rather than assumed. Without it the two drift, and the symptom is
    # two pictures of one building that disagree about where a wall is.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/StorySliceUnion.cpp"
    # ArchViz/StorySliceGeometry.cpp turns that union into what is actually
    # drawn. Both halves fail as PICTURES: a mirrored ribbon still outlines the
    # right walls, and an inside-out fill still covers a plausible region. The
    # tests assert the AREA the fill covers and the arc length the dash rides on
    # -- properties an incorrect implementation cannot accidentally satisfy.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/StorySliceGeometry.cpp"
    # ArchViz/OcclusionPrepass.cpp (PLAT-RE151) is the gate on the depth-only
    # pass that lets Grasshopper preview content go BEHIND Archicad's building.
    # It qualifies twice over. It is pure -- no Diligent, no DevKit, no Win32 --
    # and it is the ArchViz reason in its sharpest form: three of its four
    # answers are PICTURES with no error attached. Occluding over the PLAN hides
    # the preview behind storeys a plan does not show, which reads as the preview
    # transport failing; occluding in a shaded mode silently doubles the model's
    # vertex work for an identical frame; and refusing to occlude in wireframe is
    # the reported bug itself -- a sphere inside a room looking stuck to the
    # glass.
    # âš ï¸ THE GPU HALF IS ArchViz/DiligentSceneOcclusion.cpp AND IS DELIBERATELY
    # ABSENT, exactly as GhPreviewLayer is absent beside GhPreviewGeometry: it
    # owns a pipeline state and a draw loop and decides nothing.
    "${EVP_ROOT}/Sources/AddOn/ArchViz/OcclusionPrepass.cpp")

# The CloudCompare command builder is deliberately DevKit-free. It pins the
# external CLI contract while the runner itself remains in the ACAPI-free
# Python process-boundary source root.
set (EVP_CLOUDCOMPARE_SOURCES
     "${EVP_ROOT}/Sources/AddOn/Python/CloudCompareCommand.cpp")

set (EVP_ANNOTATION_SOURCES
     "${EVP_ROOT}/Sources/AddOn/Annotation/DrawList.cpp"
     "${EVP_ROOT}/Sources/AddOn/ArchViz/TraceAnnotationLayer.cpp")

set (EVP_PREVIEW_SOURCES
     "${EVP_ROOT}/Sources/AddOn/Preview/RetainedPreviewStore.cpp"
     "${EVP_ROOT}/Sources/AddOn/Preview/RetainedTraceSelection.cpp"
     "${EVP_ROOT}/Sources/AddOn/Preview/PreviewRuntimeState.cpp"
     # Preview/GhPreviewCache.cpp is the host's mirror of what Grasshopper
     # previews, and every rule in it fails SILENTLY in Archicad: a half-applied
     # batch is a picture of a building that never existed, a stale-epoch batch
     # draws geometry from a process that was killed, and a missed checksum
     # leaves the viewport permanently disagreeing with the canvas until someone
     # reopens the definition. None of the three produces an error, a log line,
     # or anything on screen a user could report as a fault.
     # âš ï¸ KEEP IT GPU-FREE, DevKit-FREE AND Win32-FREE. It is deliberately the
     # CPU half; the GPU half is ArchViz/GhPreviewLayer and the handover is
     # SceneCmdQueue. The moment this file needs any of the three it leaves this
     # list, and the offline gate on the delta protocol goes with it.
     "${EVP_ROOT}/Sources/AddOn/Preview/GhPreviewCache.cpp"
     # Preview/GhPreviewIngest.cpp is what the host DOES with a preview message
     # once the codec has decoded it: the order rules, the capability gate, the
     # shared-memory segment's lifetime, and what has to be written back down the
     # pipe. It is here for the same reason the cache is -- every failure on this
     # path is silent. A segment left mapped leaks a handle once per solve, and a
     # slider drag is hundreds of solves; an ack that is not sent leaks the
     # WORKER's segment instead; an ack sent early releases memory the worker is
     # still writing into.
     # âš ï¸ THE Win32 HALF IS Preview/GhPreviewSegmentView.cpp AND IS DELIBERATELY
     # ABSENT. It is the only file on this path that names a Win32 type, which is
     # exactly why the state machine reaches it through an interface: the tests
     # hand that interface a buffer.
     "${EVP_ROOT}/Sources/AddOn/Preview/GhPreviewIngest.cpp"
     # ArchViz/GhPreviewGeometry.cpp turns a preview snapshot into drawables, and
     # is pure for the reason every other ArchViz geometry file is: getting it
     # wrong produces a PICTURE rather than an error. A primitive routed to the
     # wrong surface draws plan linework in the model; an x-ray primitive left in
     # the depth-tested bucket is simply invisible inside the wall it exists to be
     # compared against; an unordered buffer flickers under any transparency.
     # âš ï¸ THE GPU HALF IS ArchViz/GhPreviewLayer.cpp AND IS DELIBERATELY ABSENT --
     # it owns nothing but buffers, PSOs and draws.
     "${EVP_ROOT}/Sources/AddOn/Preview/GraphPreviewStore.cpp"
     "${EVP_ROOT}/Sources/AddOn/ArchViz/GhPreviewGeometry.cpp"
     "${EVP_ROOT}/Sources/AddOn/PlanOverlay/OverlayOwnership.cpp"
     # PlanOverlay/OverlayBinding.cpp decides which Archicad window an overlay
     # belongs to and whether this tick should cost anything. Silent both ways:
     # an overlay that keeps polling while a schedule is in front looks exactly
     # like one that does not, and one that suspends when it should not simply
     # never appears.
     "${EVP_ROOT}/Sources/AddOn/PlanOverlay/OverlayBinding.cpp"
     "${EVP_ROOT}/Sources/AddOn/PlanOverlay/PlanTransformMath.cpp")

set (EVP_TEST_SOURCES
    test_drawlist.cpp
    test_traceannotationlayer.cpp
    test_retainedpreviewstore.cpp
    test_watchdrawlist.cpp
    test_overlayownership.cpp
    test_overlaybinding.cpp
    test_planoverlaytransform.cpp
    test_previewplancamera.cpp
    test_previewpanelstate.cpp
    test_storysliceunion.cpp
    test_storyslicegeometry.cpp
    test_automaticpreview.cpp
    test_command_filter.cpp
    test_param_layout.cpp
    test_param_visibility.cpp
    test_slice.cpp
    test_clash.cpp
    test_query.cpp
    test_spatial.cpp
    test_serializer.cpp
    test_render.cpp
    test_normals.cpp
    test_geometryengine.cpp
    test_meshgroups.cpp
    test_wireframeedges.cpp
    test_scenecmdqueue.cpp
    ${EVP_SCENE_TEXT_TESTS}
    test_materialtable.cpp
    test_surfaceclassifier.cpp
    test_buildingmaterialsignal.cpp
    test_substancejoin.cpp
    test_autoexposure.cpp
    test_diligentclear.cpp
    test_matrixmath.cpp
    test_debugcubemesh.cpp
    test_axisgnomonmesh.cpp
    test_sunshadowmath.cpp
    test_environmentlighting.cpp
    test_plancameramath.cpp
    test_plananchorribbon.cpp
    test_pickvote.cpp
    test_occlusionprepass.cpp
    test_pointcloudply.cpp
    test_pointcloudhierarchy.cpp
    test_pointcloudlod.cpp
    test_cloudcompare.cpp
    test_sunstudyraster.cpp
    test_queryoccluded.cpp
    test_cputraversal.cpp
    test_apipipeprotocol.cpp
    test_ghprotocol.cpp
    test_ghpreviewprotocol.cpp
    test_ghpreviewcache.cpp
    test_ghpreviewingest.cpp
    test_ghpreviewgeometry.cpp
    test_ghundobudget.cpp
    test_grasshopperhoststate.cpp
    test_nodegraph.cpp
    test_nodegraph_script.cpp
    test_datatree.cpp
    test_datatreeops.cpp
    test_treeserialization.cpp
    test_treeiteration.cpp
    test_anytree.cpp
    test_nodelifting.cpp
    test_treenodes.cpp
    test_listnodes.cpp
    test_elementclassification.cpp)

# ---- googletest (vendored source drop) -------------------------------------
# reference/ is gitignored, so a fresh clone has neither dependency. Say so here
# rather than letting the compiler fail on a missing header.
if (NOT EXISTS "${GTEST_DIR}/src/gtest-all.cc")
    message (FATAL_ERROR
        "GoogleTest not found at ${GTEST_DIR}.\n"
        "reference/ is gitignored â€” provision it once with "
        "tools/build/provision-reference.ps1 -FromUpstream -Profile test. "
        "See tests/cpp/README.md.")
endif ()
if (NOT EXISTS "${NANORT_INC}/nanort.h")
    message (FATAL_ERROR "nanort not found at ${NANORT_INC} â€” see AddOn/EvP/README.md.")
endif ()
if (NOT EXISTS "${GLM_INC}/glm/glm.hpp")
    message (FATAL_ERROR "GLM not found at ${GLM_INC} â€” see AddOn/EvP/README.md.")
endif ()
if (NOT EXISTS "${QUICKJS_DIR}/quickjs.c")
    message (FATAL_ERROR "quickjs-ng not found at ${QUICKJS_DIR} â€” run tools/build/provision-reference.ps1 -FromUpstream.")
endif ()

if (NOT EXISTS "${CLIPPER2_DIR}/Clipper2Lib/src/clipper.engine.cpp")
    message (FATAL_ERROR "Clipper2 not found at ${CLIPPER2_DIR} â€” see AddOn/EvP/README.md.")
endif ()

add_library (gtest_vendored STATIC
    "${GTEST_DIR}/src/gtest-all.cc"
    "${GTEST_DIR}/src/gtest_main.cc")
target_include_directories (gtest_vendored SYSTEM PUBLIC "${GTEST_DIR}/include")
target_include_directories (gtest_vendored PRIVATE "${GTEST_DIR}")
target_compile_features (gtest_vendored PUBLIC cxx_std_20)
if (MSVC)
    target_compile_options (gtest_vendored PRIVATE /EHsc /wd4996)
endif ()
find_package (Threads REQUIRED)
target_link_libraries (gtest_vendored PUBLIC Threads::Threads)

# See the add-on's CMakeLists for why this is quickjs-ng and why it is built from
# source in both graphs.
add_library (TapiocaQuickJs STATIC
    "${QUICKJS_DIR}/quickjs.c"
    "${QUICKJS_DIR}/libregexp.c"
    "${QUICKJS_DIR}/libunicode.c"
    "${QUICKJS_DIR}/cutils.c"
    "${QUICKJS_DIR}/xsum.c")
target_include_directories (TapiocaQuickJs SYSTEM PUBLIC "${QUICKJS_DIR}")
# âš ï¸ quickjs-libc.c IS DELIBERATELY ABSENT, and it is the security boundary rather
# than an oversight. Upstream gates it behind QJS_BUILD_LIBC; it is what provides
# `std` and `os` - file reads, writes, process spawning - to scripts. A script
# node sees its inputs and nothing else, so the library that would give it a
# filesystem must never be compiled in. test_nodegraph_script.cpp asserts `std`
# and `os` are undefined, so adding it here fails a test rather than quietly
# widening what a graph can do.
target_compile_definitions (TapiocaQuickJs PRIVATE _GNU_SOURCE)
if (WIN32)
    target_compile_definitions (TapiocaQuickJs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif ()
# C11 for the atomics and _Generic quickjs-ng uses; it is a C library and must
# not be dragged into the C++ compiler by a stray target_compile_features.
set_target_properties (TapiocaQuickJs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if (MSVC)
    # âš ï¸ /std:c11 AND /experimental:c11atomics ARE BOTH REQUIRED, and C_STANDARD 11
    # above does not supply them. quickjs.c includes <stdatomic.h>, which MSVC's
    # own vcruntime_c11_stdatomic.h refuses with `#error "C atomics require C11 or
    # later"` unless the language flag is on - and MSVC still gates the atomics
    # themselves behind the experimental switch, which is what upstream's
    # CMakeLists does too. Without them the very first file fails to compile.
    target_compile_options (TapiocaQuickJs PRIVATE /std:c11 /experimental:c11atomics)
    # Third-party C compiled at our warning level otherwise; its warnings are not
    # ours to fix and would bury our own.
    target_compile_options (TapiocaQuickJs PRIVATE /W0 -D_CRT_SECURE_NO_WARNINGS)
endif ()

add_library (TapiocaClipper2 STATIC
    "${CLIPPER2_DIR}/Clipper2Lib/src/clipper.engine.cpp"
    "${CLIPPER2_DIR}/Clipper2Lib/src/clipper.offset.cpp"
    "${CLIPPER2_DIR}/Clipper2Lib/src/clipper.rectclip.cpp"
    "${CLIPPER2_DIR}/Clipper2Lib/src/clipper.triangulation.cpp")
target_compile_features (TapiocaClipper2 PUBLIC cxx_std_20)
target_compile_definitions (TapiocaClipper2 PUBLIC CLIPPER2_MAX_DECIMAL_PRECISION=8)
target_include_directories (TapiocaClipper2 SYSTEM PUBLIC "${CLIPPER2_DIR}/Clipper2Lib/include")

# ---- the test binary -------------------------------------------------------
add_executable (EvPGeomTests ${EVP_GEOM_SOURCES} ${EVP_PALETTE_SOURCES}
                               ${EVP_ARCHVIZ_SOURCES} ${EVP_SCENE_TEXT_SOURCES} ${EVP_CLOUDCOMPARE_SOURCES}
                                ${EVP_ANNOTATION_SOURCES} ${EVP_PREVIEW_SOURCES} ${EVP_COMMAND_SOURCES}
                                ${EVP_GRASSHOPPER_SOURCES}
                                 ${EVP_DYNAMO_SOURCES}
                                 ${EVP_NODEGRAPH_SOURCES}
                                 ${EVP_SUNSTUDY_SOURCES}
                                 ${EVP_TEST_SOURCES})
target_include_directories (EvPGeomTests PRIVATE
    "${EVP_ROOT}/Sources/AddOn"
    "${GEOM_DIR}"
    "${EVP_ROOT}/Sources/AddOn/Python"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories (EvPGeomTests SYSTEM PRIVATE "${NANORT_INC}" "${GLM_INC}")
target_link_libraries (EvPGeomTests PRIVATE gtest_vendored TapiocaClipper2 TapiocaQuickJs ${TAPIOCA_TEXT_LIBS})

if (MSVC)
    # /W4 on OUR code (docs/guides/testing.md Â§5). The add-on ships at /W3; the host-free core
    # is where a stricter gate is affordable, so it starts here.
    target_compile_options (EvPGeomTests PRIVATE /W4 /EHsc /MP -D_CRT_SECURE_NO_WARNINGS)
else ()
    target_compile_options (EvPGeomTests PRIVATE -Wall -Wextra)
endif ()

if (EVP_SANITIZE)
    if (MSVC)
        # MSVC's ASan turns on STL container annotations, which change the ABI of
        # std::string/vector/optional. EVERY object in the link must agree, so the
        # flag goes on gtest as well â€” otherwise the link fails with LNK2038
        # "mismatch detected for 'annotate_string'".
        target_compile_options (EvPGeomTests    PRIVATE /fsanitize=address)
        target_compile_options (gtest_vendored  PRIVATE /fsanitize=address)
        target_compile_options (TapiocaClipper2 PRIVATE /fsanitize=address)
    else ()
        target_compile_options (EvPGeomTests   PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options    (EvPGeomTests   PRIVATE -fsanitize=address,undefined)
        target_compile_options (gtest_vendored PRIVATE -fsanitize=address,undefined)
        target_link_options    (gtest_vendored PRIVATE -fsanitize=address,undefined)
        target_compile_options (TapiocaClipper2 PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif ()
endif ()

# Where the real-dump fixtures live, so a test can find them without a hard-coded
# absolute path. Tests SKIP (not fail) when a fixture is absent, so a fresh clone
# without dumps still goes green.
# EVP_TEST_FIXTURE_DIR is the SMALL, COMMITTED fixtures beside the tests, as
# distinct from EVP_DUMP_DIR's large captures that may be absent on a fresh
# machine. The real-sky fixture lives there because an environment projection
# cannot be validated on synthetic input alone (CLAUDE.md), and a 98 KB
# downsample of a real HDR probe is small enough to track.
target_compile_definitions (EvPGeomTests PRIVATE
    EVP_DUMP_DIR="${EVP_ROOT}/../../dump"
    EVP_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    # âš ï¸ THE SHIPPED EXAMPLES ARE THE FIXTURE, RATHER THAN A COPY OF THEM. They
    # are what a user points a node at first, so their headers ARE the contract
    # this feature makes; a private copy beside the tests would let the two drift
    # and the examples are the half that would rot. An example whose header stops
    # parsing should fail the suite, because it has stopped being an example.
    EVP_TEST_SCRIPT_DIR="${EVP_ROOT}/GraphScripts")
if (EVP_TEXT_RENDERING)
    target_compile_definitions (EvPGeomTests PRIVATE EVP_SCENE_TEXT_FONT="${TAPIOCA_TEXT_FONT}")
endif ()

enable_testing ()
include (GoogleTest)
gtest_discover_tests (EvPGeomTests DISCOVERY_TIMEOUT 60)
