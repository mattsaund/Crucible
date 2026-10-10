cmake_minimum_required(VERSION 3.24)
project(crucible
    VERSION     0.8.9
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

# ---------------------------------------------------------------------------
# MSVC, for everything -- the dependencies included
#
# Up here because add_compile_options only reaches targets created after it,
# and the dependency step below is what creates llama.cpp. These used to sit
# further down, after every target in this file, and so applied to nothing.
#
# /Zc:__cplusplus is the one llama.cpp needs. Without it MSVC reports
# __cplusplus as 199711 whatever standard it is compiling, and llama-chat.cpp
# picks the type of its u8"" literals by that number: built as C++20 it got
# char8_t where it expected char, and did not compile.
#
# Scoped to C and C++ the way llama.cpp scopes its own, so nothing reaches the
# resource compiler, which takes neither.
# ---------------------------------------------------------------------------
if(MSVC)
    # The sources contain UTF-8 -- the crucible sprite's melt, the diamonds the
    # interface draws with. MSVC assumes the system code page without this
    # and mangles every one of them.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
endif()

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


# Needed before the dependencies are configured: ggml decides where to install
# its backend modules while it is being added, so CRUCIBLE_LIBDIR has to exist by
# then.
include(GNUInstallDirs)
set(CRUCIBLE_LIBDIR ${CMAKE_INSTALL_LIBDIR}/crucible)

# How a binary names the directory it is in, for the library path baked into
# it. See "Where the loader looks" in the install section.
if(APPLE)
    set(CRUCIBLE_ORIGIN "@loader_path")
else()
    set(CRUCIBLE_ORIGIN "$ORIGIN")
endif()

include(cmake/CrucibleDependencies.cmake)

# MSVC, for Crucible's own targets only. After the dependencies, so windows.h
# in their sources is left the way they expect it, and before the first target
# below, since a definition added after a target is created never reaches it.
if(MSVC)
    # windows.h defines min and max as macros, which breaks std::min the moment
    # any header reaches it.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
    # getenv, strerror and the like draw a "use the _s version" warning on
    # every call. The portable spelling is the one this code means.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# crucible_core -- everything that is not the window.
# Kept as its own library so the engine can be driven headlessly (tests, a
# future `crucible --repl`, or a daemon) without dragging a window toolkit
# along.
# ---------------------------------------------------------------------------
add_library(crucible_core STATIC
    # util: small standalone helpers with no Crucible dependencies
    src/util/text.cpp
    src/util/format.cpp
    src/util/diff.cpp
    src/util/platform.cpp
    # Reading code before drawing it: the lexer that colors a block, and the
    # reader that works out whether it is a diff and what to number each line.
    # In the core rather than in one of the faces because both draw code, and a
    # terminal that colors it differently from a window is two programs.
    # How big the desktop app draws itself. In the core rather than the desktop
    # app because the Mac is the case that went wrong, and from a Linux machine
    # a unit test is the only place it can be checked.
    src/util/resources.cpp
    src/util/http.cpp
    src/util/workers.cpp
    src/util/subprocess.cpp
    src/util/sha256.cpp
    src/lab/python.cpp
    src/orchestra/link.cpp
    src/orchestra/package.cpp

    # config: what is on disk -- settings, paths, folder trust
    src/config/paths.cpp
    src/config/config.cpp
    src/config/config_io.cpp
    src/config/gpu_policy.cpp
    src/config/trust.cpp

    # runtime: the loadable compute backends and the devices they expose
    src/runtime/backend.cpp
    src/runtime/prebuilt.cpp
    src/runtime/cuda_libraries.cpp
    src/runtime/registry.cpp
    src/runtime/builder.cpp
    src/runtime/devices.cpp

    # session: token accounting and per-project conversation history
    src/session/usage.cpp
    src/session/store.cpp

    # cook: the long-running improvement loop and the record it keeps
    src/cook/journal.cpp

    # lab: making a model rather than running one -- what a custom model is
    # made of, and browsing the hub for the pieces
    src/lab/recipe.cpp
    src/lab/hub.cpp
    src/lab/pyenv.cpp
    src/lab/trainer.cpp

    # routing: the expert roster, the two routers, and the command list
    src/routing/expert.cpp
    src/routing/router.cpp
    src/routing/benchmark.cpp

    # llm: everything that touches llama.cpp
    src/llm/model_catalog.cpp
    src/llm/model_shape.cpp
    src/llm/response_filter.cpp
    src/tools/web_search.cpp
    src/tools/attachments.cpp
    src/tools/pdf_text.cpp
    src/tools/office_binary.cpp
    src/tools/workshop.cpp
    # tools, beyond the folder: version control, the web, the screen, programs
    # left running, and a page of the project made showable
    src/tools/git.cpp
    src/tools/fetch.cpp
    src/tools/computer.cpp
    src/tools/processes.cpp
    src/tools/preview.cpp
    src/tools/apps.cpp
    src/tools/mcp.cpp
    src/kit/kit.cpp
    src/app/starter.cpp
    src/app/self_source.cpp
    src/app/relocate.cpp
    src/llm/sampling.cpp
    src/llm/loaded_model.cpp
    src/llm/remote_model.cpp
    src/llm/spend.cpp
    src/llm/prices.cpp
    src/llm/mlx_server.cpp
    src/llm/model_host.cpp

    # engine: the delegation loop and the state the UI reads
    src/engine/overflow.cpp
    src/engine/state.cpp
    src/engine/engine.cpp
    src/engine/engine_cook.cpp
    src/engine/leases.cpp

    # app: process-level actions that are not the window
    # api: the one JSON door every interface comes through
    src/api/surface.cpp
    src/api/wire.cpp
    src/api/methods_conversation.cpp
    src/api/methods_project.cpp
    src/api/methods_roster.cpp
    src/api/methods_settings.cpp
    src/api/methods_install.cpp
    src/api/methods_lab.cpp
    src/api/methods_source.cpp

    src/app/cli.cpp
    src/app/uninstall.cpp
    src/app/setup_trainer.cpp
    src/app/setup_runtimes.cpp
    src/app/setup.cpp
    src/app/update.cpp
)
add_library(crucible::core ALIAS crucible_core)

target_include_directories(crucible_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(crucible_core
    PUBLIC  llama nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads crucible_miniz)
# Pictures, for a model on this machine that has a projector beside it. See
# LoadedModel::chat_with_pictures.
if(TARGET mtmd)
    target_link_libraries(crucible_core PRIVATE mtmd)
    target_compile_definitions(crucible_core PRIVATE CRUCIBLE_HAS_MTMD)
endif()
# ShellExecuteW: opening a program Crucible made as the person's own, not as
# a child of Crucible's. See src/tools/apps.cpp.
if(WIN32)
    target_link_libraries(crucible_core PRIVATE shell32)
endif()

# The orchestrator package, compiled in. See src/orchestra/package.cpp.
target_sources(crucible_core PRIVATE ${CRUCIBLE_ORCHESTRATOR_CPP})

# The fine-tuning script, compiled in. Absent only if someone deleted it from
# the source tree; the trainer then reports that rather than writing a file
# that is not there.
if(CRUCIBLE_TRAINER_CPP)
    target_sources(crucible_core PRIVATE ${CRUCIBLE_TRAINER_CPP})
    target_compile_definitions(crucible_core PUBLIC CRUCIBLE_HAS_TRAINER_SCRIPT)
else()
    message(WARNING "scripts/trainer/finetune.py not found; training will be unavailable")
endif()

# The notices, compiled in so `crucible --licenses` can print them on a
# machine that has only the binary.
if(CRUCIBLE_NOTICES_CPP)
    target_sources(crucible_core PRIVATE ${CRUCIBLE_NOTICES_CPP})
    target_compile_definitions(crucible_core PUBLIC CRUCIBLE_HAS_NOTICES)
else()
    message(WARNING "packaging/licenses/NOTICES.txt not found; "
                    "crucible --licenses will have nothing to print")
endif()

target_compile_definitions(crucible_core PUBLIC
    CRUCIBLE_VERSION="${PROJECT_VERSION}"
    # The runtime builder compiles GPU backends from this exact llama.cpp tag.
    # A backend built from a different one would load and then crash on the
    # first tensor, so the tag the binary was built against travels with it.
    CRUCIBLE_LLAMA_TAG="${CRUCIBLE_LLAMA_TAG}")

# On Windows, a program's plain strings are read in the ANSI code page unless
# its manifest says UTF-8 -- and every string Crucible has is UTF-8. See
# packaging/windows/crucible.manifest.
function(crucible_utf8_manifest target)
    if(MSVC)
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/crucible.manifest)
    else()
        target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/manifest.rc)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# crucible -- the program
#
# One binary, and it is the window. There was a terminal face beside it sharing
# this engine; it is gone rather than left to rot half-maintained, and the
# history has it if it is ever wanted back. Everything it knew lives in
# crucible::core, which is why removing it took no engine with it.
# ---------------------------------------------------------------------------
add_executable(crucible
    src/gui/main.cpp
    src/gui/app.cpp
    src/gui/crash.cpp
    src/gui/window.cpp)
target_link_libraries(crucible PRIVATE crucible::core)

# The platform's own file dialog: one function, and the implementation that
# belongs to wherever this is being built. See src/gui/dialogs.hpp. And the
# paths of files dropped on the window, which only GTK will say: see
# src/gui/drops.hpp.
if(WIN32)
    target_sources(crucible PRIVATE src/gui/dialogs_win.cpp src/gui/drops_none.cpp)
    # IFileOpenDialog is COM: ole32 makes the object, uuid names its class.
    # dbghelp: a crash's stack by name, in crash.log. See src/gui/crash.hpp.
    target_link_libraries(crucible PRIVATE ole32 uuid shell32 dbghelp)
    crucible_utf8_manifest(crucible)
    # The icon, inside the program. See packaging/windows/icon.rc.in.
    set(CRUCIBLE_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/crucible.ico")
    configure_file(packaging/windows/icon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/crucible_icon.rc" @ONLY)
    target_sources(crucible PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/crucible_icon.rc")
elseif(APPLE)
    target_sources(crucible PRIVATE src/gui/dialogs_mac.cpp src/gui/drops_none.cpp)
    target_link_libraries(crucible PRIVATE "-framework AppKit")
else()
    target_sources(crucible PRIVATE src/gui/dialogs_gtk.cpp src/gui/drops_gtk.cpp)
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
    # crucible-smoke: one prompt through the whole engine with no window, for
    # a platform's prompt path where a window cannot be opened.
    add_executable(crucible-smoke tools/smoke.cpp)
    target_link_libraries(crucible-smoke PRIVATE crucible::core)
    if(CRUCIBLE_WARNINGS AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
        target_compile_options(crucible-smoke PRIVATE ${_crucible_warnings})
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
        tests/test_transcript_edits.cpp
        tests/test_overflow.cpp
        tests/test_api.cpp
        tests/test_http.cpp
        tests/test_remote.cpp
        tests/test_lab.cpp
        tests/test_trainer.cpp
        tests/test_prebuilt.cpp
        tests/test_update.cpp
        tests/test_attachments.cpp
        tests/test_spelling.cpp
        tests/test_orchestra.cpp
        tests/test_build.cpp
        tests/test_source.cpp
        tests/test_kit.cpp
        tests/test_mcp.cpp)
    target_link_libraries(crucible_tests PRIVATE crucible::core)
    # The interface's markdown, syntax coloring and diffs are JavaScript now.
    # They are tested by running ui/render.js in JavaScriptCore -- the same
    # engine the webview uses -- rather than left untested for being on the
    # other side of the language boundary.
    if(CRUCIBLE_JSC_FOUND AND CRUCIBLE_RENDER_CPP)
        target_sources(crucible_tests PRIVATE tests/test_ui_js.cpp
                       ${CRUCIBLE_RENDER_CPP} ${CRUCIBLE_WEBUI_CPP})
        target_include_directories(crucible_tests PRIVATE ${JSC_INCLUDE_DIRS})
        target_link_libraries(crucible_tests PRIVATE ${JSC_LIBRARIES})
        target_link_directories(crucible_tests PRIVATE ${JSC_LIBRARY_DIRS})
    endif()
    # dbghelp turns a crash address into a function name, for the report
    # tests/test_main.cpp prints on Windows -- where a crash otherwise reaches
    # the log as one line from CTest and nothing else.
    if(WIN32)
        target_link_libraries(crucible_tests PRIVATE dbghelp)
        # The tests run with the code page the program does, so the one that
        # opens a file by a UTF-8 path checks the manifest as well.
        crucible_utf8_manifest(crucible_tests)
    endif()
    target_include_directories(crucible_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # So the spelling lint can walk the project from wherever it is run.
    target_compile_definitions(crucible_tests
        PRIVATE CRUCIBLE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
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
# ELF and Mach-O take a search path baked into the binary, relative to the
# binary itself -- so bin/../lib/crucible resolves whether the prefix is
# ~/.local or /usr/local, and `crucible` runs from anywhere on PATH with no
# LD_LIBRARY_PATH. The two spell "the directory this binary is in" differently:
# $ORIGIN on Linux, @loader_path on macOS. macOS reads $ORIGIN as the name of a
# directory that does not exist, so an installed crucible there could not find
# libllama.dylib and would not start. CRUCIBLE_ORIGIN, set near the top, is
# whichever this platform uses.
#
# Windows has no such thing at all: it looks beside the executable, so there
# the DLLs are installed into bin/ and nothing has to be baked in.
if(CRUCIBLE_BACKEND_DL AND NOT WIN32)
    set_target_properties(crucible PROPERTIES
        BUILD_RPATH   "${CRUCIBLE_ORIGIN}/../${CRUCIBLE_LIBDIR}"
        INSTALL_RPATH "${CRUCIBLE_ORIGIN}/../${CRUCIBLE_LIBDIR}")
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
# --- the desktop entry ------------------------------------------------------
#
# What puts Crucible in the application menu. Linux only: macOS wants an .app
# bundle and Windows a Start Menu shortcut, and neither is a .desktop file --
# both are made by the installers.
#
# The Exec line is the absolute installed path rather than the bare name. The
# usual prefix is ~/.local, which a login shell puts on PATH and a desktop
# launcher very often does not -- a bare "crucible" there is an icon that
# silently does nothing when clicked.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CRUCIBLE_GUI_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/crucible")
    configure_file(packaging/linux/crucible.desktop.in
                   "${CMAKE_CURRENT_BINARY_DIR}/crucible.desktop" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crucible.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
    # hicolor/scalable is the theme every desktop falls back to, so one SVG
    # covers every icon size the panel, dock and menu ask for.
    install(FILES packaging/icons/crucible.svg
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
endif()

# The webview interface, when this build has one. Added to the program rather
# than to the core library: it is a way of drawing Crucible, and crucible_core
# is what any other way of driving it would share.
if(CRUCIBLE_HAS_WEBVIEW AND CRUCIBLE_WEBUI_CPP)
    target_sources(crucible PRIVATE ${CRUCIBLE_WEBUI_CPP})
    target_compile_definitions(crucible PRIVATE CRUCIBLE_HAS_WEB_INDEX CRUCIBLE_HAS_WEBVIEW)
    target_link_libraries(crucible PRIVATE webview::core_static)
    # The page draws in the same typeface the window did and shows the same
    # flame, both handed over as data URIs rather than hoped for from the
    # system or looked for on disk.
    if(TARGET crucible_fonts)
        target_link_libraries(crucible PRIVATE crucible_fonts)
    endif()
    if(CRUCIBLE_MARK_CPP)
        target_sources(crucible PRIVATE ${CRUCIBLE_MARK_CPP})
        target_compile_definitions(crucible PRIVATE CRUCIBLE_HAS_EMBEDDED_MARK)
    endif()
    message(STATUS "web interface: compiled in")
else()
    message(FATAL_ERROR
        "The interface could not be built, and it is the program's only one.\n"
        "On Debian and Ubuntu: sudo apt-get install libwebkit2gtk-4.1-dev\n"
        "On Fedora: sudo dnf install webkit2gtk4.1-devel")
endif()

# crucible-app: the window a program Crucible made runs in, once it is
# packaged -- a page, its name, and a place for its data. Built with Crucible
# and installed beside it, because packaging an app is copying this next to
# the app's page. Nothing of Crucible's own is linked in: no models, no
# engine, so the program it becomes is small and runs on a machine that has
# never had Crucible on it. See tools/app_runner.cpp and src/tools/apps.cpp.
if(CRUCIBLE_HAS_WEBVIEW)
    add_executable(crucible-app tools/app_runner.cpp)
    target_link_libraries(crucible-app PRIVATE webview::core_static nlohmann_json::nlohmann_json)
    if(WIN32)
        set_target_properties(crucible-app PROPERTIES WIN32_EXECUTABLE TRUE)
        # UTF-8 for its narrow strings, as Crucible's own: the data folder is
        # under a home folder whose name may be anybody's.
        crucible_utf8_manifest(crucible-app)
    endif()
    install(TARGETS crucible-app
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT ${CRUCIBLE_INSTALL_COMPONENT})
    # Packaging is tested with the real runner beside the test binary.
    if(TARGET crucible_tests)
        add_dependencies(crucible_tests crucible-app)
    endif()
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
    foreach(_lib llama ggml ggml-base mtmd)
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
if(CRUCIBLE_BACKEND_DL)
    message(STATUS "  runtimes      : loadable, none built in -- add them from settings")
else()
    message(STATUS "  runtimes      : compiled in -- CUDA=${CRUCIBLE_CUDA} Vulkan=${CRUCIBLE_VULKAN}")
    if(NOT CRUCIBLE_CUDA AND NOT CRUCIBLE_VULKAN)
        message(STATUS "  -> CPU-only build with no way to add a GPU backend later.")
    endif()
endif()
message(STATUS "")
