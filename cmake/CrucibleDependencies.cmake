# ---------------------------------------------------------------------------
# Third-party dependencies.
#
# Everything is fetched at configure time and pinned to an exact tag so a
# fresh clone always builds the same thing. To move a dependency forward,
# change the tag here and nothing else.
#
# Set FETCHCONTENT_SOURCE_DIR_<NAME> to point at a local checkout if you want
# to develop against one without re-downloading, e.g.
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_LLAMA=/path/to/llama.cpp
# ---------------------------------------------------------------------------
include(FetchContent)

find_package(Threads REQUIRED)

set(CRUCIBLE_LLAMA_TAG b10678     CACHE STRING "llama.cpp git tag to build against")
set(CRUCIBLE_JSON_TAG  v3.12.0    CACHE STRING "nlohmann/json git tag to build against")
set(CRUCIBLE_IMGUI_TAG v1.91.9b   CACHE STRING "Dear ImGui git tag to build against")
set(CRUCIBLE_GLFW_TAG  3.4        CACHE STRING "GLFW git tag, used only when the system has none")
set(CRUCIBLE_FONT_TAG  2.304      CACHE STRING "JetBrains Mono release to compile into the binary")

# ---------------------------------------------------------------------------
# Symlink-free shared libraries
#
# See cmake/CrucibleUnversion.cmake. In short: llama.cpp's shared objects would
# normally be written as libggml-base.so.0.9.4 plus a libggml-base.so symlink,
# and a build tree on exFAT or NTFS cannot hold the symlink. Crucible strips the
# versioning instead, so the build works on any filesystem on any platform.
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/CrucibleUnversion.cmake)

# ---------------------------------------------------------------------------
# Dear ImGui, GLFW and nlohmann/json are always static: they are Crucible's own
# code as far as deployment is concerned, and there is no reason to ship them as
# separate files. Only llama.cpp is built shared, and only when runtimes are
# loadable.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)

# --- nlohmann/json ---------------------------------------------------------
# Header-only; used for the config file, the trust store and session history.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${CRUCIBLE_JSON_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# The desktop window: GLFW and Dear ImGui. Not optional -- the window is the
# program.
#
# Only fetched when the GUI is being built, because they are the one dependency
# that needs anything from the system -- OpenGL and, on Linux, the X11 or
# Wayland development headers. Someone who only wants `crucible` in a terminal
# should not have to install those to get it.
#
# ImGui rather than Qt or a web stack, and the reason is the core library.
# Crucible's engine is C++ and the whole point of the desktop app is that it is
# the same program with a different face -- same Engine, same roster, same cook
# loop, no protocol in between. ImGui links straight against it and produces one
# self-contained binary. Qt would mean a system dependency an order of magnitude
# larger; Electron or Tauri would mean a second language and an IPC layer whose
# only job is to undo the fact that the engine is already right there.
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)

# The system's GLFW when there is one -- it is a small, stable library and
# distributions package it well. Building our own is the fallback, and needs
# the X11 development headers that install.sh asks for.
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    set(GLFW_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(GLFW_BUILD_TESTS    OFF CACHE INTERNAL "")
    set(GLFW_BUILD_DOCS     OFF CACHE INTERNAL "")
    set(GLFW_INSTALL        OFF CACHE INTERNAL "")

    # GLFW 3.4 builds both display backends on Linux by default, and the
    # Wayland one is generated code: without wayland-scanner and the protocol
    # XML its configure step stops with "Failed to find wayland-scanner" -- on a
    # machine that has every X11 header asked for and would have built fine.
    #
    # So the backend follows the tooling that is actually installed rather than
    # being demanded: both when it is there, X11 alone when it is not. An X11
    # build still runs on a Wayland desktop through XWayland, which is the trade
    # being made when this says "X11 only".
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(CRUCIBLE_WAYLAND_SCANNER wayland-scanner)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            # Everything GLFW's Wayland backend compiles against, not just the
            # generator: the protocol XML, the client library and the keyboard
            # handling. Any one of them missing is the same failed configure.
            pkg_check_modules(CRUCIBLE_WAYLAND QUIET
                              wayland-protocols>=1.15 wayland-client xkbcommon)
        endif()
        if(CRUCIBLE_WAYLAND_SCANNER AND CRUCIBLE_WAYLAND_FOUND)
            set(GLFW_BUILD_WAYLAND ON  CACHE INTERNAL "")
            message(STATUS "GLFW: building X11 and Wayland backends")
        else()
            set(GLFW_BUILD_WAYLAND OFF CACHE INTERNAL "")
            message(STATUS "GLFW: building the X11 backend only "
                           "(no wayland-scanner or wayland-protocols)")
        endif()
        set(GLFW_BUILD_X11 ON CACHE INTERNAL "")
    endif()
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        ${CRUCIBLE_GLFW_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(glfw)
    message(STATUS "GLFW: building from source (no system package found)")
else()
    message(STATUS "GLFW: using the system package")
endif()

# ImGui ships no CMakeLists of its own, so the sources are named here. Only
# the two backends Crucible uses are compiled in.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${CRUCIBLE_IMGUI_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(imgui)

add_library(crucible_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    # std::string overloads for the input widgets. Without these every text
    # box needs a fixed char buffer and its own resize dance, which is a
    # great deal of ceremony for a name and a description.
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(crucible_imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(crucible_imgui PUBLIC glfw OpenGL::GL)

# --- the interface font -------------------------------------------------
#
# JetBrains Mono, compiled in rather than looked for. A font is the one
# asset the program cannot draw for itself, and searching for it at runtime
# would mean an install layout and a search path for a typeface -- the same
# machinery the flame mark avoids by being vector shapes. Three faces, which
# is what rendering markdown needs: prose, **bold**, and *italic*.
#
# Fetched and pinned like everything else. If the download fails the build
# still works and the app falls back to whatever monospace face the system
# has, because a missing typeface is not a reason to have no program.
FetchContent_Declare(jetbrains_mono
    URL      https://github.com/JetBrains/JetBrainsMono/releases/download/v${CRUCIBLE_FONT_TAG}/JetBrainsMono-${CRUCIBLE_FONT_TAG}.zip
    URL_HASH SHA256=6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(jetbrains_mono)

set(CRUCIBLE_FONT_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CRUCIBLE_FONT_DIR})

set(CRUCIBLE_FONT_SOURCES "")
set(CRUCIBLE_FONTS_EMBEDDED ON)
foreach(_face Regular Bold Italic)
    string(TOLOWER ${_face} _symbol)
    set(_ttf ${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-${_face}.ttf)
    set(_cpp ${CRUCIBLE_FONT_DIR}/font_${_symbol}.cpp)
    if(NOT EXISTS ${_ttf})
        message(WARNING "JetBrains Mono ${_face} not found; the desktop app will "
                        "fall back to a system font")
        set(CRUCIBLE_FONTS_EMBEDDED OFF)
        break()
    endif()
    # Configure time, not build time: the input never changes, so there is
    # nothing for a dependency rule to track.
    if(NOT EXISTS ${_cpp} OR ${_ttf} IS_NEWER_THAN ${_cpp})
        execute_process(COMMAND ${CMAKE_COMMAND}
            -DIN=${_ttf} -DOUT=${_cpp} -DSYMBOL=k${_face}
            -P ${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake
            RESULT_VARIABLE _embed_status)
        if(NOT _embed_status EQUAL 0)
            message(WARNING "could not embed JetBrains Mono ${_face}")
            set(CRUCIBLE_FONTS_EMBEDDED OFF)
            break()
        endif()
    endif()
    list(APPEND CRUCIBLE_FONT_SOURCES ${_cpp})
endforeach()

if(CRUCIBLE_FONTS_EMBEDDED)
    add_library(crucible_fonts STATIC ${CRUCIBLE_FONT_SOURCES})
    target_compile_definitions(crucible_fonts PUBLIC CRUCIBLE_HAS_EMBEDDED_FONT)
    target_link_libraries(crucible_imgui PUBLIC crucible_fonts)
endif()

# --- llama.cpp -------------------------------------------------------------
# We link libllama directly and use the raw C API, so none of llama.cpp's own
# binaries or its `common` helper library are needed. Turning them off saves a
# large amount of build time and drops the libcurl dependency.
set(LLAMA_BUILD_TESTS    OFF CACHE INTERNAL "")
set(LLAMA_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(LLAMA_BUILD_TOOLS    OFF CACHE INTERNAL "")
set(LLAMA_BUILD_SERVER   OFF CACHE INTERNAL "")
set(LLAMA_BUILD_APP      OFF CACHE INTERNAL "")
set(LLAMA_BUILD_COMMON   OFF CACHE INTERNAL "")
set(LLAMA_CURL           OFF CACHE INTERNAL "")

set(GGML_BUILD_TESTS     OFF CACHE INTERNAL "")
set(GGML_BUILD_EXAMPLES  OFF CACHE INTERNAL "")
if(CRUCIBLE_BACKEND_DL)
    # The whole point of this mode: ggml gains the ability to dlopen a backend
    # at startup, so CUDA and Vulkan become files in a directory the settings
    # screen manages rather than a decision frozen at compile time. ggml
    # refuses to build this way against static archives, so llama.cpp -- and
    # only llama.cpp -- is shared here.
    set(BUILD_SHARED_LIBS ON)
    set(GGML_BACKEND_DL ON  CACHE INTERNAL "")

    # No backend at all is compiled here -- not even the CPU one.
    #
    # Crucible installs with an empty runtimes directory and the settings screen
    # builds whichever backends you ask for, which is what makes the choice
    # reversible. Building the CPU modules here would produce a dozen shared
    # objects that the install rules would then have to leave behind, and it
    # roughly quadruples the time the installer spends compiling.
    #
    # GGML_NATIVE has to go with it: ggml rejects it outright in DL mode,
    # because a module chosen at run time cannot be compiled for whatever CPU
    # happened to build it. The runtime builder passes GGML_CPU_ALL_VARIANTS
    # instead, which emits one module per x86-64 feature level and lets ggml
    # score them at load.
    set(GGML_NATIVE           OFF CACHE INTERNAL "")
    set(GGML_CPU              OFF CACHE INTERNAL "")
    set(GGML_CPU_ALL_VARIANTS OFF CACHE INTERNAL "")

    # GGML_BACKEND_DIR is deliberately not set. It would bake an absolute
    # search path into the binary at build time -- wrong the moment the install
    # is relocated -- and its install rule lands outside Crucible's own install
    # component. The runtimes directory is passed to ggml at startup instead,
    # by RuntimeRegistry::load_all().

    set(GGML_CUDA       OFF CACHE INTERNAL "")
    set(GGML_VULKAN     OFF CACHE INTERNAL "")

    # These three are not off by default on a Mac. ggml sets GGML_METAL_DEFAULT
    # and GGML_BLAS_DEFAULT to ON under APPLE, and GGML_ACCELERATE is ON
    # everywhere, so a build whose entire purpose is to contain no backend
    # would arrive containing two -- compiled, and then left in bin/ for the
    # install rules to ignore, while the settings screen goes on to build Metal
    # a second time. Naming them costs nothing on Linux, where they are already
    # off, and is the difference between the design holding and not on macOS.
    set(GGML_METAL      OFF CACHE INTERNAL "")
    set(GGML_BLAS       OFF CACHE INTERNAL "")
    set(GGML_ACCELERATE OFF CACHE INTERNAL "")
else()
    # Monolithic fallback: one static binary with at most one GPU backend
    # compiled in. Nothing is loadable and the runtime manager reports itself
    # as unavailable, but it builds anywhere, including on exFAT.
    set(BUILD_SHARED_LIBS OFF)
    set(GGML_NATIVE     ${CRUCIBLE_NATIVE} CACHE INTERNAL "")
    set(GGML_BACKEND_DL OFF           CACHE INTERNAL "")
    set(GGML_CPU        ON            CACHE INTERNAL "")
    set(GGML_CUDA       ${CRUCIBLE_CUDA}   CACHE INTERNAL "")
    set(GGML_VULKAN     ${CRUCIBLE_VULKAN} CACHE INTERNAL "")
endif()

FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${CRUCIBLE_LLAMA_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)

FetchContent_MakeAvailable(llama)

# llama, ggml, ggml-base and every backend module, stripped of their version
# suffixes. Safe in the monolithic build too, where all of them are static
# archives and there is nothing to strip.
crucible_unversion_directory("${llama_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Treat every dependency's headers as system headers, so Crucible can keep a
# strict warning set without drowning in diagnostics from llama.cpp and ImGui.
# ---------------------------------------------------------------------------
foreach(_dep llama ggml ggml-base ggml-cpu nlohmann_json)
    if(TARGET ${_dep})
        # ALIAS targets reject set_target_properties, so resolve through them.
        get_target_property(_aliased ${_dep} ALIASED_TARGET)
        if(_aliased)
            set(_real ${_aliased})
        else()
            set(_real ${_dep})
        endif()
        set_target_properties(${_real} PROPERTIES SYSTEM TRUE)
    endif()
endforeach()
