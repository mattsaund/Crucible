# Assemble the interface into one document.
#
# The page is written as a page: ui/index.html names a stylesheet and a list
# of scripts the ordinary way, with <link> and <script src>, so each is a file
# with one job and an editor that knows what it is. But a webview is handed a
# single string, not a directory, so at build time every reference is replaced
# by the file it names.
#
# No bundler, no node, no npm: the only tool is the cmake that is already
# building everything else. What that costs is modules -- the scripts share
# one global scope, in the order index.html lists them, exactly as they would
# if a browser loaded them.
#
# Run with -P: cmake -DIN=ui/index.html -DOUT=build/.../page.html -P BundlePage.cmake
# A script run with -P starts with no policies at all, which makes `while(TRUE)`
# a warning about what TRUE might mean. This says: the modern meaning.
cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED IN OR NOT DEFINED OUT)
    message(FATAL_ERROR "BundlePage.cmake needs IN and OUT")
endif()

get_filename_component(_dir "${IN}" DIRECTORY)
file(READ "${IN}" _page)

# Where a reference is read from. ui/ for the page's own files; VENDOR for
# what index.html names under vendor/ -- KaTeX, which the build prepares
# rather than the repository carrying. A vendor file that is not there is
# left out, and the page does without it: a build that could not fetch KaTeX
# shows math as its TeX source, which is not a reason to fail the build.
function(_bundle_source name out_var)
    if(name MATCHES "^vendor/(.*)$")
        if(DEFINED VENDOR AND EXISTS "${VENDOR}/${CMAKE_MATCH_1}")
            set(${out_var} "${VENDOR}/${CMAKE_MATCH_1}" PARENT_SCOPE)
        else()
            set(${out_var} "" PARENT_SCOPE)
        endif()
        return()
    endif()
    if(NOT EXISTS "${_dir}/${name}")
        message(FATAL_ERROR "ui/index.html names ${name}, which is not there")
    endif()
    set(${out_var} "${_dir}/${name}" PARENT_SCOPE)
endfunction()

# <link rel="stylesheet" href="x.css">  ->  <style> ... </style>
while(TRUE)
    string(REGEX MATCH "<link rel=\"stylesheet\" href=\"([^\"]+)\">" _tag "${_page}")
    if(NOT _tag)
        break()
    endif()
    _bundle_source("${CMAKE_MATCH_1}" _file)
    if(_file)
        file(READ "${_file}" _text)
        string(REPLACE "${_tag}" "<style>\n${_text}</style>" _page "${_page}")
    else()
        string(REPLACE "${_tag}" "" _page "${_page}")
    endif()
endwhile()

# <script src="x.js"></script>  ->  <script> ... </script>
while(TRUE)
    string(REGEX MATCH "<script src=\"([^\"]+)\"></script>" _tag "${_page}")
    if(NOT _tag)
        break()
    endif()
    set(_name "${CMAKE_MATCH_1}")
    _bundle_source("${_name}" _file)
    if(_file)
        file(READ "${_file}" _text)
        # The file's name, so a syntax error reported against the bundle can be
        # traced back to the file it came from.
        string(REPLACE "${_tag}" "<script>\n// ---- ${_name} ----\n${_text}</script>"
               _page "${_page}")
    else()
        string(REPLACE "${_tag}" "" _page "${_page}")
    endif()
endwhile()

file(WRITE "${OUT}" "${_page}")
