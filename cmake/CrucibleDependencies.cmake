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
# Several things are compiled in from files someone edits: the trainer script,
# the orchestrator, the interface's page, the window's mark and the
# third-party notices. A custom command
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

# The llama.cpp this version is made for. Not a cache variable: a build folder
# kept across an update -- the installer's is the checkout's own -- remembered
# the tag it was first configured with and went on building that one. The
# override below builds against another. (runtimes.yml reads the tag from the
# first line here that names it.)
set(CRUCIBLE_LLAMA_TAG b11846)
set(CRUCIBLE_LLAMA_TAG_OVERRIDE "" CACHE STRING "llama.cpp git tag to build against instead of this version's")
if(CRUCIBLE_LLAMA_TAG_OVERRIDE)
    set(CRUCIBLE_LLAMA_TAG ${CRUCIBLE_LLAMA_TAG_OVERRIDE})
endif()
set(CRUCIBLE_JSON_TAG  v3.12.0    CACHE STRING "nlohmann/json git tag to build against")
set(CRUCIBLE_WEBVIEW_TAG 0.12.0   CACHE STRING "webview git tag for the web interface")
set(CRUCIBLE_KATEX_TAG 0.19.0     CACHE STRING "KaTeX release the page draws math with")

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

# --- generated sources ------------------------------------------------------
#
# What the build makes from the files it compiles in -- crucible_embed's
# sources, above, and the bundled page -- goes here.
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
               "routing.py:Routing" "cook.py:Cook" "naming.py:Naming" "build.py:Build"
               "chat.py:Chat" "bench.py:Bench" "bench_score.py:BenchScore" "intent.py:Intent")
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

# --- math ------------------------------------------------------------------
#
# KaTeX is how the page draws math, and it comes as a script, a stylesheet and
# twenty fonts. The script and the stylesheet are inlined into the page like
# the page's own (see BundlePage.cmake); the fonts go in as bytes, and the
# window turns them into @font-face rules. The stylesheet's own rules for them
# point at files beside it, which do not exist inside a binary, so they are
# taken out here.
#
# The interface itself has no typeface of its own: it is drawn in the
# machine's, or in whichever one the Font setting names.
#
# Fetched and pinned like everything else, and embedded at configure time
# rather than build time: the inputs never change, so there is nothing for a
# dependency rule to track. A font that will not embed is a warning, not a
# failed build: the page then shows math as its TeX source.
FetchContent_Declare(katex
    URL      https://github.com/KaTeX/KaTeX/releases/download/v${CRUCIBLE_KATEX_TAG}/katex.tar.gz
    URL_HASH SHA256=966d9c85655081cca9a57e8a1d667f13f96b1c85c3f93655a8acef74c4ce3875
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(katex)

set(CRUCIBLE_FONT_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${CRUCIBLE_FONT_DIR})

# Embed one file as `symbol` in `name_space`, unless it already is.
function(crucible_embed_now source generated symbol name_space ok_var)
    set(${ok_var} ON PARENT_SCOPE)
    if(EXISTS "${generated}" AND NOT "${source}" IS_NEWER_THAN "${generated}")
        return()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND}
        "-DIN=${source}" "-DOUT=${generated}" "-DSYMBOL=${symbol}" "-DNAMESPACE=${name_space}"
        -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedBinary.cmake
        RESULT_VARIABLE _status)
    if(NOT _status EQUAL 0)
        set(${ok_var} OFF PARENT_SCOPE)
    endif()
endfunction()

set(CRUCIBLE_FONT_SOURCES "")

# KaTeX's fonts, and a table of them: the family, weight and style of each
# are in its file name, the same way KaTeX's stylesheet spells them out --
# KaTeX_Main-BoldItalic is KaTeX_Main at 700, italic.
set(CRUCIBLE_KATEX_DIR ${CRUCIBLE_FONT_DIR}/vendor)
file(MAKE_DIRECTORY ${CRUCIBLE_KATEX_DIR})
file(GLOB _katex_fonts "${katex_SOURCE_DIR}/fonts/KaTeX_*.woff2")
list(SORT _katex_fonts)
set(CRUCIBLE_KATEX_EMBEDDED OFF)
if(_katex_fonts AND EXISTS "${katex_SOURCE_DIR}/katex.min.js")
    set(CRUCIBLE_KATEX_EMBEDDED ON)
    set(_declarations "")
    set(_rows "")
    set(_n 0)
    foreach(_font IN LISTS _katex_fonts)
        get_filename_component(_name "${_font}" NAME_WE)            # KaTeX_Main-BoldItalic
        string(REGEX MATCH "^KaTeX_([A-Za-z0-9]+)-([A-Za-z]+)$" _parts "${_name}")
        set(_family "KaTeX_${CMAKE_MATCH_1}")
        set(_variant "${CMAKE_MATCH_2}")
        set(_weight 400)
        set(_italic false)
        if(_variant MATCHES "Bold")
            set(_weight 700)
        endif()
        if(_variant MATCHES "Italic")
            set(_italic true)
        endif()
        crucible_embed_now("${_font}" ${CRUCIBLE_FONT_DIR}/katex_font_${_n}.cpp
                           kFont${_n} crucible::gui::katex _embedded)
        if(NOT _embedded)
            set(CRUCIBLE_KATEX_EMBEDDED OFF)
            break()
        endif()
        list(APPEND CRUCIBLE_FONT_SOURCES ${CRUCIBLE_FONT_DIR}/katex_font_${_n}.cpp)
        string(APPEND _declarations
            "extern const unsigned char kFont${_n}[];\nextern const unsigned int kFont${_n}_size;\n")
        string(APPEND _rows "    {\"${_family}\", ${_weight}, ${_italic}, kFont${_n}, kFont${_n}_size},\n")
        math(EXPR _n "${_n} + 1")
    endforeach()
endif()
if(CRUCIBLE_KATEX_EMBEDDED)
    file(CONFIGURE OUTPUT ${CRUCIBLE_FONT_DIR}/katex_fonts.cpp CONTENT
"// Generated by cmake/CrucibleDependencies.cmake from KaTeX ${CRUCIBLE_KATEX_TAG}. Do not edit.
#include \"katex_fonts.hpp\"

namespace crucible::gui::katex {
@_declarations@
const Face kFaces[] = {
@_rows@};
const unsigned int kFaceCount = ${_n}u;
}  // namespace crucible::gui::katex
" @ONLY)
    list(APPEND CRUCIBLE_FONT_SOURCES ${CRUCIBLE_FONT_DIR}/katex_fonts.cpp)

    # The page's half: the script as it ships, and the stylesheet without the
    # rules that point at files. Two of KaTeX's own additions with it: copying
    # a formula copies its TeX rather than the letters it is drawn with, and
    # mhchem, for the \ce{H2O} chemistry is written in.
    configure_file("${katex_SOURCE_DIR}/katex.min.js" ${CRUCIBLE_KATEX_DIR}/katex.min.js COPYONLY)
    configure_file("${katex_SOURCE_DIR}/contrib/copy-tex.min.js" ${CRUCIBLE_KATEX_DIR}/copy-tex.min.js COPYONLY)
    configure_file("${katex_SOURCE_DIR}/contrib/mhchem.min.js" ${CRUCIBLE_KATEX_DIR}/mhchem.min.js COPYONLY)
    file(READ "${katex_SOURCE_DIR}/katex.min.css" _katex_css)
    string(REGEX REPLACE "@font-face{[^}]*}" "" _katex_css "${_katex_css}")
    # Verbatim, and only when it changed, so the page is not rebundled on
    # every configure.
    set(_was "")
    if(EXISTS ${CRUCIBLE_KATEX_DIR}/katex.css)
        file(READ ${CRUCIBLE_KATEX_DIR}/katex.css _was)
    endif()
    if(NOT _was STREQUAL _katex_css)
        file(WRITE ${CRUCIBLE_KATEX_DIR}/katex.css "${_katex_css}")
    endif()
else()
    message(WARNING "KaTeX could not be embedded; math will show as its TeX source")
    file(REMOVE ${CRUCIBLE_KATEX_DIR}/katex.min.js ${CRUCIBLE_KATEX_DIR}/katex.css
                ${CRUCIBLE_KATEX_DIR}/copy-tex.min.js ${CRUCIBLE_KATEX_DIR}/mhchem.min.js)
endif()

if(CRUCIBLE_KATEX_EMBEDDED)
    add_library(crucible_fonts STATIC ${CRUCIBLE_FONT_SOURCES})
    target_include_directories(crucible_fonts PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src/gui)
    target_compile_definitions(crucible_fonts PUBLIC CRUCIBLE_HAS_KATEX)
endif()

# --- third-party notices -------------------------------------------------
#
# Compiled in, because they have to travel with the thing they are notices
# for. A dmg, an exe and an AppImage each carry the binary and not the source
# tree, and the licenses of what is compiled in -- KaTeX among them -- want
# their notices to go wherever the code goes.
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
    ${CRUCIBLE_UI_DIR}/preview.js
    ${CRUCIBLE_UI_DIR}/build.js
    ${CRUCIBLE_UI_DIR}/code.js
    ${CRUCIBLE_UI_DIR}/create.js
    ${CRUCIBLE_UI_DIR}/history.js
    ${CRUCIBLE_UI_DIR}/models.js
    ${CRUCIBLE_UI_DIR}/bench.js
    ${CRUCIBLE_UI_DIR}/settings.js)
set(CRUCIBLE_WEBUI_CPP  "")
set(CRUCIBLE_RENDER_CPP "")
if(EXISTS ${CRUCIBLE_UI_DIR}/index.html)
    set(CRUCIBLE_PAGE_HTML ${CRUCIBLE_GENERATED_DIR}/page.html)
    # What index.html names under vendor/ comes from the build folder: KaTeX,
    # prepared above. Without it the page is bundled without math.
    set(_vendor_files "")
    if(CRUCIBLE_KATEX_EMBEDDED)
        set(_vendor_files ${CRUCIBLE_KATEX_DIR}/katex.min.js ${CRUCIBLE_KATEX_DIR}/katex.css
                          ${CRUCIBLE_KATEX_DIR}/copy-tex.min.js ${CRUCIBLE_KATEX_DIR}/mhchem.min.js)
    endif()
    add_custom_command(
        OUTPUT  ${CRUCIBLE_PAGE_HTML}
        COMMAND ${CMAKE_COMMAND}
                -DIN=${CRUCIBLE_UI_DIR}/index.html -DOUT=${CRUCIBLE_PAGE_HTML}
                -DVENDOR=${CRUCIBLE_KATEX_DIR}
                -P ${CMAKE_CURRENT_LIST_DIR}/BundlePage.cmake
        DEPENDS ${CRUCIBLE_UI_FILES} ${_vendor_files} ${CMAKE_CURRENT_LIST_DIR}/BundlePage.cmake
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
# And KaTeX beside it, so the tests can draw math with the script the page has.
set(CRUCIBLE_KATEX_JS_CPP "")
if(CRUCIBLE_KATEX_EMBEDDED)
    crucible_embed(CRUCIBLE_KATEX_JS_CPP ${CRUCIBLE_KATEX_DIR}/katex.min.js
                   kKatexJs crucible::gui::web web_katex)
endif()

# One target makes them. The window and the tests both compile the page, and
# two targets that list a custom command's output each run that command --
# under make, at the same time: one compiled the page while the other was
# halfway through writing it, and the array came out empty. Both depend on
# this instead, so it is made once, first.
add_custom_target(crucible_page
    DEPENDS ${CRUCIBLE_WEBUI_CPP} ${CRUCIBLE_RENDER_CPP} ${CRUCIBLE_KATEX_JS_CPP})

# Those same functions, run by the tests in the engine that will run them for
# real. JavaScriptCore ships with WebKitGTK, so on a Linux machine that can
# build the interface it is already here; on a Mac it is a system framework
# with the same C interface, because WKWebView is WebKit too. Only Windows,
# whose webview is not WebKit, skips that one file -- and every other test
# still runs there.
set(CRUCIBLE_JSC_FOUND OFF)
if(APPLE)
    find_library(CRUCIBLE_JSC_FRAMEWORK JavaScriptCore)
    if(CRUCIBLE_JSC_FRAMEWORK)
        set(JSC_LIBRARIES ${CRUCIBLE_JSC_FRAMEWORK})
        set(JSC_INCLUDE_DIRS "")
        set(JSC_LIBRARY_DIRS "")
        set(CRUCIBLE_JSC_FOUND ON)
    endif()
elseif(UNIX)
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
# mtmd alone, without llama.cpp's tools: the library a model that sees pictures
# needs to read one -- its projector -- and nothing that links `common`.
set(LLAMA_BUILD_MTMD     ON  CACHE INTERNAL "")

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

# --- cpp-httplib: the API server ---------------------------------------------
#
# The OpenAI-compatible server (src/serve/server.cpp) speaks HTTP through
# cpp-httplib, which llama.cpp carries in its vendor folder for its own server
# and builds only with LLAMA_BUILD_COMMON, which is off here. Compiled from
# that copy rather than fetched: it is the one this llama.cpp tag pins, and
# nothing new comes down for it. Without TLS, as LM Studio's server and
# llama-server are: it answers on this machine, or on its network when asked.
set(CRUCIBLE_HTTPLIB_DIR "${llama_SOURCE_DIR}/vendor/cpp-httplib")
add_library(crucible_httplib STATIC "${CRUCIBLE_HTTPLIB_DIR}/httplib.cpp")
target_include_directories(crucible_httplib SYSTEM PUBLIC "${CRUCIBLE_HTTPLIB_DIR}")
target_link_libraries(crucible_httplib PRIVATE Threads::Threads)
set_target_properties(crucible_httplib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Its warnings are its own business, as llama.cpp's build says too.
if(MSVC)
    target_compile_options(crucible_httplib PRIVATE /w)
else()
    target_compile_options(crucible_httplib PRIVATE -w)
endif()
# Winsock. MSVC is told by a pragma in the header; MinGW is not.
if(WIN32)
    target_link_libraries(crucible_httplib PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Treat every dependency's headers as system headers, so Crucible can keep a
# strict warning set without drowning in diagnostics from llama.cpp.
# ---------------------------------------------------------------------------
foreach(_dep llama ggml ggml-base ggml-cpu mtmd nlohmann_json)
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
