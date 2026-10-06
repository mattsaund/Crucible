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

# --- embedding a source file into the binary -----------------------------
#
# Three things are compiled in from files someone edits: the trainer script,
# the interface's page, and the third-party notices. A custom command
# rather than execute_process, because execute_process runs at configure time
# -- so editing one of them and rebuilding did nothing until cmake was rerun,
# which is a very quiet way to spend an hour wondering why an edit had no
# effect. DEPENDS is the rule make and ninja actually track.
function(crucible_embed out_var source symbol name_space label)
    set(generated ${CRUCIBLE_GENERATED_DIR}/${label}.cpp)
    add_custom_command(
        OUTPUT  ${generated}
        COMMAND ${CMAKE_COMMAND}
                -DIN=${source} -DOUT=${generated}
                -DSYMBOL=${symbol} -DNAMESPACE=${name_space}
                -P ${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake
        DEPENDS ${source} ${CMAKE_CURRENT_LIST_DIR}/EmbedBinary.cmake
        COMMENT "Embedding ${label}"
        VERBATIM)
    set(${out_var} ${generated} PARENT_SCOPE)
endfunction()

find_package(Threads REQUIRED)

set(CRUCIBLE_LLAMA_TAG b10678     CACHE STRING "llama.cpp git tag to build against")
set(CRUCIBLE_JSON_TAG  v3.12.0    CACHE STRING "nlohmann/json git tag to build against")
set(CRUCIBLE_WEBVIEW_TAG 0.12.0   CACHE STRING "webview git tag for the web interface")
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
# webview and nlohmann/json are always static: they are Crucible's own code as
# far as deployment is concerned, and there is no reason to ship them as
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

# --- miniz: zip archives and deflate ----------------------------------------
#
# For reading what somebody attaches to a prompt. A .docx, an .xlsx, a .pptx,
# an .odt and an .epub are all zip files of XML, and the text in a PDF is
# almost always deflate-compressed. One small C file does both, under the MIT
# license, and is compiled in rather than looked for: zlib is on every Linux
# machine and on neither of the other two.
#
# The release archive rather than the repository, because the archive is the
# single amalgamated file -- the repository is the pieces it is made from and
# the build that makes it.
set(CRUCIBLE_MINIZ_TAG 3.1.2 CACHE STRING "miniz release to compile in")
FetchContent_Declare(miniz
    URL      https://github.com/richgel999/miniz/releases/download/${CRUCIBLE_MINIZ_TAG}/miniz-${CRUCIBLE_MINIZ_TAG}.zip
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz)
add_library(crucible_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(crucible_miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR})
# No time functions: nothing here writes an archive, and they are the part
# that differs between platforms. And none of zlib's names -- `inflate`,
# `compress`, `crc32` -- which miniz otherwise defines in the global
# namespace, where they would sit beside Crucible's own.
target_compile_definitions(crucible_miniz PUBLIC MINIZ_NO_TIME MINIZ_NO_ARCHIVE_WRITING_APIS
                                                 MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(crucible_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# The window
# ---------------------------------------------------------------------------
#
# There is no window toolkit here. The interface is a page, drawn by the
# webview the platform already has, so the only dependency is the thin header
# that opens one.

# webview is one MIT header wrapping the webview each platform already has:
# WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux. Nothing is
# embedded and no browser is shipped -- it is the same relationship Crucible
# had with OpenGL, which was also a thing the machine provided.
#
# Required, because it is the only interface. A machine without the headers
# cannot build the program at all, so this says so with the command that
# fixes it rather than producing a binary with no window.
set(CRUCIBLE_HAS_WEBVIEW OFF)
set(_webview_ok TRUE)
if(UNIX AND NOT APPLE)
    # WebKitGTK is the one platform where this is a package rather than part
    # of the system, so it is the one that can be missing.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WEBKIT2 QUIET webkit2gtk-4.1)
        if(NOT WEBKIT2_FOUND)
            pkg_check_modules(WEBKIT2 QUIET webkit2gtk-4.0)
        endif()
    endif()
    if(NOT WEBKIT2_FOUND)
        set(_webview_ok FALSE)
    endif()
endif()

if(_webview_ok)
    # Static only, and no symlinked sonames. The shared build writes
    # libwebview.so.0.12 as a symlink to the real file, which an exFAT build
    # directory cannot hold -- the same wall llama.cpp hit, solved there by
    # CrucibleUnversionHook.cmake. A static library sidesteps it and suits the
    # single-binary story better anyway.
    set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE INTERNAL "")
    set(WEBVIEW_BUILD_STATIC_LIBRARY ON  CACHE INTERNAL "")
    set(WEBVIEW_BUILD_TESTS          OFF CACHE INTERNAL "")
    set(WEBVIEW_BUILD_EXAMPLES       OFF CACHE INTERNAL "")
    set(WEBVIEW_BUILD_DOCS           OFF CACHE INTERNAL "")
    set(WEBVIEW_INSTALL_TARGETS      OFF CACHE INTERNAL "")

    FetchContent_Declare(webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG        ${CRUCIBLE_WEBVIEW_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(webview)
    set(CRUCIBLE_HAS_WEBVIEW ON)
    message(STATUS "web interface: building against the system webview")
endif()
# --- the window's mark ----------------------------------------------------
#
# The flame in the corner of the window, and the same artwork the application
# icon is made from. Compiled in for the same reason the font is: looking for
# it on disk at runtime would mean an install layout and a search path for a
# picture.
#
# Raw RGBA rather than the PNG, because nothing in Crucible can decode a PNG --
set(CRUCIBLE_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CRUCIBLE_GENERATED_DIR})


# --- the trainer script -------------------------------------------------
#
# Fine-tuning is a Python script, and it is compiled in for the same reason
# the mark is: an AppImage, a .app bundle and a Windows install put their data
# files in three different places, and a trainer that is always exactly the
# one this build expects beats one that could be looked for, found stale, or
# edited. It is written out beside its virtual environment when that is
# installed, so upgrading Crucible upgrades the trainer.
# The orchestrator -- routing and the cook loop -- is Python too, and is
# compiled in for the same reason the trainer is: the package written out at
# startup is always exactly the one this build expects. src/orchestra/package.cpp
# lists the same files.
set(CRUCIBLE_ORCHESTRATOR_DIR ${CMAKE_CURRENT_LIST_DIR}/../scripts/orchestrator/crucible_orchestrator)
set(CRUCIBLE_ORCHESTRATOR_CPP "")
foreach(_entry "__init__.py:Init" "__main__.py:Run" "main.py:Main" "rpc.py:Rpc"
               "routing.py:Routing" "cook.py:Cook" "naming.py:Naming")
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _symbol)
    crucible_embed(_generated ${CRUCIBLE_ORCHESTRATOR_DIR}/${_file}
                   kOrchestrator${_symbol} crucible::orchestra::embedded orchestrator_${_symbol})
    list(APPEND CRUCIBLE_ORCHESTRATOR_CPP ${_generated})
endforeach()

set(CRUCIBLE_TRAINER_PY  ${CMAKE_CURRENT_LIST_DIR}/../scripts/trainer/finetune.py)
set(CRUCIBLE_TRAINER_CPP "")
if(EXISTS ${CRUCIBLE_TRAINER_PY})
    crucible_embed(CRUCIBLE_TRAINER_CPP ${CRUCIBLE_TRAINER_PY}
                   kFinetunePy crucible::lab::embedded trainer_script)
endif()

# --- third-party notices -------------------------------------------------
#
# Compiled in, because they have to travel with the thing they are notices
# for. A dmg, an exe and an AppImage each carry the binary and not the source
# tree, and JetBrains Mono's license requires its notice to accompany the font
# wherever the font goes -- and the font goes inside the binary.
set(CRUCIBLE_NOTICES_TXT ${CMAKE_CURRENT_LIST_DIR}/../packaging/licenses/NOTICES.txt)
set(CRUCIBLE_NOTICES_CPP "")
if(EXISTS ${CRUCIBLE_NOTICES_TXT})
    crucible_embed(CRUCIBLE_NOTICES_CPP ${CRUCIBLE_NOTICES_TXT}
                   kNotices crucible::app::embedded notices)
endif()

# --- the interface's page -------------------------------------------------
#
# Compiled in for the same reason the trainer script is: an AppImage, a .app
# bundle and a Windows install put their data in three different places, and a
# page that is always exactly the one this build expects beats one that could
# be looked for and found stale.
#
# It is several files under ui/ -- a stylesheet, and a script per view -- and
# one document by the time it is embedded: see cmake/BundlePage.cmake. The
# list is here rather than globbed so that adding a file is a line somebody
# wrote, and so that the order the scripts load in is the order index.html
# names them and nowhere else.
set(CRUCIBLE_UI_DIR ${CMAKE_CURRENT_LIST_DIR}/../ui)
set(CRUCIBLE_UI_FILES
    ${CRUCIBLE_UI_DIR}/index.html
    ${CRUCIBLE_UI_DIR}/style.css
    ${CRUCIBLE_UI_DIR}/render.js
    ${CRUCIBLE_UI_DIR}/base.js
    ${CRUCIBLE_UI_DIR}/shell.js
    ${CRUCIBLE_UI_DIR}/attach.js
    ${CRUCIBLE_UI_DIR}/chat.js
    ${CRUCIBLE_UI_DIR}/cook.js
    ${CRUCIBLE_UI_DIR}/create.js
    ${CRUCIBLE_UI_DIR}/history.js
    ${CRUCIBLE_UI_DIR}/settings.js)
set(CRUCIBLE_WEBUI_CPP  "")
set(CRUCIBLE_RENDER_CPP "")
if(EXISTS ${CRUCIBLE_UI_DIR}/index.html)
    set(CRUCIBLE_PAGE_HTML ${CRUCIBLE_GENERATED_DIR}/page.html)
    add_custom_command(
        OUTPUT  ${CRUCIBLE_PAGE_HTML}
        COMMAND ${CMAKE_COMMAND}
                -DIN=${CRUCIBLE_UI_DIR}/index.html -DOUT=${CRUCIBLE_PAGE_HTML}
                -P ${CMAKE_CURRENT_LIST_DIR}/BundlePage.cmake
        DEPENDS ${CRUCIBLE_UI_FILES} ${CMAKE_CURRENT_LIST_DIR}/BundlePage.cmake
        COMMENT "Bundling the interface"
        VERBATIM)
    crucible_embed(CRUCIBLE_WEBUI_CPP ${CRUCIBLE_PAGE_HTML}
                   kPageHtml crucible::gui::web web_page)
endif()
# The text-to-markup half on its own as well, for the tests: it is the part
# that is a pure function of its input, and tests/test_ui_js.cpp runs exactly
# these bytes without a document around them.
if(EXISTS ${CRUCIBLE_UI_DIR}/render.js)
    crucible_embed(CRUCIBLE_RENDER_CPP ${CRUCIBLE_UI_DIR}/render.js
                   kRenderJs crucible::gui::web web_render)
endif()

# Those same functions, run by the tests in the engine that will run them for
# real. JavaScriptCore ships with WebKitGTK, so on a machine that can build
# the interface it is already here; where it is not -- Windows and macOS,
# whose webviews are not WebKit-on-GTK -- the suite skips that one file and
# every other test still runs.
set(CRUCIBLE_JSC_FOUND OFF)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JSC QUIET javascriptcoregtk-4.1)
        if(NOT JSC_FOUND)
            pkg_check_modules(JSC QUIET javascriptcoregtk-4.0)
        endif()
        if(JSC_FOUND)
            set(CRUCIBLE_JSC_FOUND ON)
        endif()
    endif()
endif()

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
endif()

# The flame in the corner of the window. The SVG rather than the raw RGBA the
# old window used: a webview can draw one of those and not the other, and it
# is the same artwork either way.
set(CRUCIBLE_MARK_SVG ${CMAKE_CURRENT_LIST_DIR}/../packaging/icons/crucible.svg)
set(CRUCIBLE_MARK_CPP "")
if(EXISTS ${CRUCIBLE_MARK_SVG})
    crucible_embed(CRUCIBLE_MARK_CPP ${CRUCIBLE_MARK_SVG}
                   kMarkSvg crucible::gui::art icon_mark)
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
# strict warning set without drowning in diagnostics from llama.cpp.
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
