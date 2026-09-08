cmake_minimum_required(VERSION 3.24)
project(crucible
    VERSION     0.1.0
    DESCRIPTION "A local AI engine that delegates to expert models and cooks on projects over time"
    LANGUAGES   C CXX)

# ---------------------------------------------------------------------------
# Standards & defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Keep binaries in one predictable place: build/bin/crucible
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Crucible options
# ---------------------------------------------------------------------------
# CRUCIBLE_BACKEND_DL is the normal way to build. ggml gets its dlopen-based
# backend loader, the CPU backend is emitted as a loadable module next to the
# binary, and CUDA/Vulkan are built later and on demand by the settings screen.
# Turning it OFF gives the old monolithic build: one static binary with at most
# one backend frozen in, selected by CRUCIBLE_CUDA / CRUCIBLE_VULKAN below. That
# mode exists for anyone who wants a single self-contained executable and is
# willing to decide the backend at compile time. It is no longer the fallback
# for filesystems without symlinks -- the loadable build works there now, see
# cmake/CrucibleUnversion.cmake.
option(CRUCIBLE_BACKEND_DL "Build with loadable GPU runtimes (recommended)" ON)

# Only consulted when CRUCIBLE_BACKEND_DL is OFF:
#   CRUCIBLE_CUDA   -> needs the CUDA toolkit (nvcc)
#   CRUCIBLE_VULKAN -> needs the Vulkan SDK + glslc + SPIR-V headers
option(CRUCIBLE_CUDA     "Monolithic build only: compile in the CUDA backend"   OFF)
option(CRUCIBLE_VULKAN   "Monolithic build only: compile in the Vulkan backend" OFF)
option(CRUCIBLE_NATIVE   "Tune the build for this exact CPU"       ON)
option(CRUCIBLE_WARNINGS "Enable a strict warning set for Crucible's own sources" ON)
option(CRUCIBLE_BUILD_TESTS "Build the unit tests" ON)
option(CRUCIBLE_BUILD_TOOLS "Build the developer tools (routebench)" ON)

# The desktop application. Off by default because it is the one part of
# Crucible that needs anything from the system -- OpenGL, and on Linux the X11
# or Wayland development headers -- and someone who only wants the terminal
# program should not have to install those to get it. install.sh turns it on
# when the headers are there.
option(CRUCIBLE_BUILD_GUI "Build the desktop application (crucible-gui)" OFF)

# Needed before the dependencies are configured: ggml decides where to install
# its backend modules while it is being added, so CRUCIBLE_LIBDIR has to exist by
# then.
include(GNUInstallDirs)
set(CRUCIBLE_LIBDIR ${CMAKE_INSTALL_LIBDIR}/crucible)

include(cmake/CrucibleDependencies.cmake)

# ---------------------------------------------------------------------------
# crucible_core -- everything that is not the terminal UI.
# Kept as its own library so the engine can be driven headlessly (tests, a
# future `crucible --repl`, or a daemon) without dragging FTXUI along.
# ---------------------------------------------------------------------------
add_library(crucible_core STATIC
    # util: small standalone helpers with no Crucible dependencies
    src/util/text.cpp
    src/util/format.cpp
    src/util/diff.cpp
    src/util/platform.cpp
    src/util/markdown.cpp
    src/util/resources.cpp
    src/util/subprocess.cpp

    # config: what is on disk -- settings, paths, folder trust
    src/config/paths.cpp
    src/config/config.cpp
    src/config/config_io.cpp
    src/config/gpu_policy.cpp
    src/config/trust.cpp

    # runtime: the loadable compute backends and the devices they expose
    src/runtime/backend.cpp
    src/runtime/registry.cpp
    src/runtime/builder.cpp
    src/runtime/devices.cpp

    # session: token accounting and per-project conversation history
    src/session/usage.cpp
    src/session/store.cpp

    # cook: the long-running improvement loop and the record it keeps
    src/cook/journal.cpp

    # routing: the expert roster, the two routers, and the command list
    src/routing/expert.cpp
    src/routing/router.cpp
    src/routing/benchmark.cpp
    src/routing/completion.cpp

    # llm: everything that touches llama.cpp
    src/llm/model_catalog.cpp
    src/llm/model_shape.cpp
    src/llm/response_filter.cpp
    src/tools/web_search.cpp
    src/tools/workshop.cpp
    src/llm/sampling.cpp
    src/llm/loaded_model.cpp
    src/llm/model_host.cpp

    # engine: the delegation loop and the state the UI reads
    src/engine/state.cpp
    src/engine/route_policy.cpp
    src/engine/engine.cpp
    src/engine/engine_cook.cpp

    # app: process-level actions that are not the TUI
    src/app/cli.cpp
    src/app/trust_gate.cpp
    src/app/uninstall.cpp
)
add_library(crucible::core ALIAS crucible_core)

target_include_directories(crucible_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(crucible_core
    PUBLIC  llama nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads)

target_compile_definitions(crucible_core PUBLIC
    CRUCIBLE_VERSION="${PROJECT_VERSION}"
    # The runtime builder compiles GPU backends from this exact llama.cpp tag.
    # A backend built from a different one would load and then crash on the
    # first tensor, so the tag the binary was built against travels with it.
    CRUCIBLE_LLAMA_TAG="${CRUCIBLE_LLAMA_TAG}")

# ---------------------------------------------------------------------------
# crucible -- the TUI binary
# ---------------------------------------------------------------------------
add_executable(crucible
    src/main.cpp
    src/ui/app.cpp
    src/ui/commands.cpp
    src/ui/transcript.cpp
    src/ui/widgets/flame_sprite.cpp
    src/ui/widgets/expert_panel.cpp
    src/ui/widgets/markdown_view.cpp
    src/ui/widgets/scroll.cpp
    src/ui/widgets/resource_meter.cpp
    src/ui/widgets/expert_form.cpp
    src/ui/settings/settings_view.cpp
    src/ui/settings/line_editor.cpp
    src/ui/settings/model_picker.cpp
    src/ui/settings/directory_browser.cpp
    src/ui/settings/runtime_view.cpp
    src/ui/settings/gpu_order_view.cpp
    src/ui/settings/model_manager_view.cpp
    src/ui/session_picker.cpp
)

target_link_libraries(crucible PRIVATE
    crucible::core
    ftxui::screen
    ftxui::dom
    ftxui::component)

# ---------------------------------------------------------------------------
# crucible-gui -- the desktop binary
#
# The same engine with a different face. It links crucible::core exactly as the
# terminal program does and knows nothing about routing, cooking or models that
# the terminal does not: if the two ever disagree about what an expert is, that
# is a bug in one of the faces rather than a difference of opinion.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BUILD_GUI)
    add_executable(crucible-gui
        src/gui/main.cpp
        src/gui/app.cpp
        src/gui/code_lines.cpp
        src/gui/markdown_view.cpp
        src/gui/syntax.cpp
        src/gui/theme.cpp
        src/gui/widgets.cpp
        src/gui/panels/topbar.cpp
        src/gui/panels/sidebar.cpp
        src/gui/panels/chat.cpp
        src/gui/panels/cook.cpp
        src/gui/panels/experts.cpp
        src/gui/panels/settings.cpp
        src/gui/panels/generation.cpp
        src/gui/panels/hardware.cpp
        src/gui/panels/runtimes.cpp
        src/gui/panels/dialogs.cpp)
    target_link_libraries(crucible-gui PRIVATE crucible::core crucible_imgui)
    if(CRUCIBLE_BACKEND_DL AND NOT WIN32)
        set_target_properties(crucible-gui PROPERTIES
            BUILD_RPATH   "$ORIGIN/../${CRUCIBLE_LIBDIR}"
            INSTALL_RPATH "$ORIGIN/../${CRUCIBLE_LIBDIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# MSVC
#
# Four settings, and every one of them is a thing that would otherwise fail
# rather than a preference.
# ---------------------------------------------------------------------------
if(MSVC)
    # The sources contain UTF-8 -- the crucible sprite's melt, the diamonds the
    # expert panel draws with. MSVC assumes the system code page without this
    # and mangles every one of them.
    add_compile_options(/utf-8)
    # windows.h defines min and max as macros, which breaks std::min the moment
    # any header reaches it.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # localtime_s and friends are the "secure" versions MSVC wants; the ones
    # this code uses are already those, but llama.cpp's are not.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Warnings -- applied only to our own targets, never to the vendored deps.
# ---------------------------------------------------------------------------
if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(_crucible_warnings
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wcast-align
        -Wunused -Woverloaded-virtual -Wdouble-promotion)
    target_compile_options(crucible_core PRIVATE ${_crucible_warnings})
    target_compile_options(crucible      PRIVATE ${_crucible_warnings})
    if(CRUCIBLE_BUILD_GUI)
        target_compile_options(crucible-gui PRIVATE ${_crucible_warnings})
    endif()
endif()

# ---------------------------------------------------------------------------
# Tools
#
# crucible-routebench measures how well a candidate delegator picks subjects,
# which is the thing that decides whether Crucible works well. Built by default
# because it is small and it is how you evaluate a new router model.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BUILD_TOOLS)
    add_executable(crucible-routebench tools/routebench.cpp)
    target_link_libraries(crucible-routebench PRIVATE crucible::core)
    if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(crucible-routebench PRIVATE ${_crucible_warnings})
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
#
# These cover the parts that need no model on disk, so `ctest` runs in under a
# second on a clean checkout and is safe to gate CI on.
# ---------------------------------------------------------------------------
if(CRUCIBLE_BUILD_TESTS)
    enable_testing()
    add_executable(crucible_tests
        tests/test_main.cpp
        tests/test_routing.cpp
        tests/test_tools.cpp
        tests/test_cook.cpp
        tests/test_config.cpp
        tests/test_devices.cpp
        tests/test_runtime.cpp
        tests/test_session.cpp
        tests/test_sprite.cpp
        tests/test_syntax.cpp
        tests/test_code_lines.cpp
        tests/test_transcript_edits.cpp
        # The flame's own source, because it lives in the terminal program
        # rather than in the core library. It draws text and nothing else, so it
        # costs the test binary a link against FTXUI's colour type and no more.
        src/ui/widgets/flame_sprite.cpp
        # And the two halves of a code block that are decisions rather than
        # drawings: the lexer that colours it, and the reader that works out
        # whether it is a diff and what to number each line. Both live in the
        # desktop program and neither includes anything from ImGui -- text in,
        # structure out, which is exactly the shape a test wants.
        src/gui/syntax.cpp
        src/gui/code_lines.cpp)
    target_link_libraries(crucible_tests PRIVATE crucible::core ftxui::screen)
    target_include_directories(crucible_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(crucible_tests PRIVATE ${_crucible_warnings})
    endif()
    add_test(NAME core COMMAND crucible_tests)
endif()

# ---------------------------------------------------------------------------
# Install
#
# A loadable-runtime build is no longer a single file: the binary sits in
# bin/ and llama.cpp's shared libraries in lib/crucible/. RPATH is what keeps
# `crucible` runnable from anywhere on PATH without LD_LIBRARY_PATH -- $ORIGIN
# is the directory of the binary itself, so bin/../lib/crucible resolves whether
# the prefix is ~/.local or /usr/local.
# ---------------------------------------------------------------------------
# Where the loader looks for llama.cpp's shared libraries.
#
# ELF and Mach-O take a search path baked into the binary, and $ORIGIN is the
# directory the binary is in -- so bin/../lib/crucible resolves whether the
# prefix is ~/.local or /usr/local, and `crucible` runs from anywhere on PATH
# with no LD_LIBRARY_PATH. Windows has no such thing: it looks beside the
# executable, so there the DLLs are installed into bin/ and nothing has to be
# baked in at all.
if(CRUCIBLE_BACKEND_DL AND NOT WIN32)
    set_target_properties(crucible PROPERTIES
        BUILD_RPATH   "$ORIGIN/../${CRUCIBLE_LIBDIR}"
        INSTALL_RPATH "$ORIGIN/../${CRUCIBLE_LIBDIR}")
endif()

# Everything Crucible installs goes in one component, and the install is run as
# `cmake --install build --component crucible`.
#
# That is not tidiness for its own sake. llama.cpp and ggml carry their own
# unconditional install rules -- libllama.so and libggml*.so straight into
# <prefix>/lib, plus ggml-config.cmake and ggml.pc -- written for people who
# are installing llama.cpp as a library. Crucible is not: it wants those shared
# objects in a private directory it can also delete again. Installing only
# this component leaves those rules unexecuted, so a system-wide install
# cannot drop a libllama.so into /usr/local/lib that shadows someone else's,
# and `crucible --uninstall` really does remove everything it put down.
set(CRUCIBLE_INSTALL_COMPONENT crucible)

install(TARGETS crucible
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
if(CRUCIBLE_BUILD_GUI)
    install(TARGETS crucible-gui
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
endif()
# --- the desktop entry ------------------------------------------------------
#
# What puts crucible-gui in the application menu. Linux only: macOS wants an
# .app bundle and Windows a Start Menu shortcut, and neither is a .desktop file.
#
# The Exec line is the absolute installed path rather than the bare name. The
# usual prefix is ~/.local, which a login shell puts on PATH and a desktop
# launcher very often does not -- a bare "crucible-gui" there is an icon that
# silently does nothing when clicked.
if(CRUCIBLE_BUILD_GUI AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CRUCIBLE_GUI_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/crucible-gui")
    configure_file(packaging/crucible.desktop.in
                   "${CMAKE_CURRENT_BINARY_DIR}/crucible.desktop" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crucible.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
    # hicolor/scalable is the theme every desktop falls back to, so one SVG
    # covers every icon size the panel, dock and menu ask for.
    install(FILES packaging/crucible.svg
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
endif()

if(CRUCIBLE_BUILD_TOOLS)
    install(TARGETS crucible-routebench
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
endif()

if(CRUCIBLE_BACKEND_DL)
    # llama.cpp's core libraries. Beside the binary on Windows, where that is
    # the only place the loader looks; in Crucible's own lib directory
    # everywhere else, where the RPATH above points at it.
    foreach(_lib llama ggml ggml-base)
        if(TARGET ${_lib})
            # These targets carry a PUBLIC_HEADER list for people who install
            # llama.cpp as a library. Crucible only wants the binary, so clear
            # it rather than scatter llama.cpp's headers through the prefix.
            set_target_properties(${_lib} PROPERTIES PUBLIC_HEADER "")
            # COMPONENT is repeated per artifact kind on purpose. Everything
            # after LIBRARY/RUNTIME applies to that kind alone, so a single
            # trailing COMPONENT would attach to RUNTIME and leave the LIBRARY
            # rule in the default "Unspecified" component -- which
            # `--install --component crucible` never installs. That is exactly
            # how these shared objects went missing: the binaries landed, the
            # libraries they need did not, and crucible would not start.
            install(TARGETS ${_lib}
                    LIBRARY DESTINATION ${CRUCIBLE_LIBDIR}
                            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT}
                    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
        endif()
    endforeach()

    # No backend modules are installed, because none are built: see the
    # GGML_CPU comment in cmake/CrucibleDependencies.cmake. A fresh install has
    # an empty runtimes directory and the settings screen fills it.
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  Crucible ${PROJECT_VERSION}")
message(STATUS "  ------------------------------------------")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  native tuning : ${CRUCIBLE_NATIVE}")
message(STATUS "  tests         : ${CRUCIBLE_BUILD_TESTS}")
if(CRUCIBLE_BUILD_GUI)
    message(STATUS "  desktop app   : yes -- crucible-gui")
else()
    message(STATUS "  desktop app   : no  -- configure with -DCRUCIBLE_BUILD_GUI=ON")
endif()
if(CRUCIBLE_BACKEND_DL)
    message(STATUS "  runtimes      : loadable, none built in -- add them from settings")
else()
    message(STATUS "  runtimes      : compiled in -- CUDA=${CRUCIBLE_CUDA} Vulkan=${CRUCIBLE_VULKAN}")
    if(NOT CRUCIBLE_CUDA AND NOT CRUCIBLE_VULKAN)
        message(STATUS "  -> CPU-only build with no way to add a GPU backend later.")
    endif()
endif()
message(STATUS "")
