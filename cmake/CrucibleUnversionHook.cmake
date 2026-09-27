# ---------------------------------------------------------------------------
# The unversioning, as something a llama.cpp build can be handed.
#
# CrucibleUnversion.cmake defines the function and stops there, because
# Crucible's own build calls it itself once FetchContent has made llama.cpp
# available. A build of llama.cpp on its own -- the release workflow's, and the
# runtime builder's -- has nowhere to make that call from, so it passes this
# file as CMAKE_PROJECT_INCLUDE instead.
#
# The call has to be deferred. CMAKE_PROJECT_INCLUDE runs immediately after
# llama.cpp's project() command, when no target exists yet and there is nothing
# to unversion; DEFER runs it at the end of that directory, by which point every
# subdirectory has been added and every library defined.
#
# Getting this wrong is quiet rather than loud. The build succeeds, and the
# modules it produces need libggml-base.so.0 -- a name Crucible does not ship,
# because Crucible's own libraries are unversioned -- so they install, fail to
# dlopen, and show up as "installed, no devices".
#
# src/runtime/builder.cpp writes the same hook at runtime, where there is no
# checkout to read this from. tests/test_install.sh keeps the two in step.
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_LIST_DIR}/CrucibleUnversion.cmake")

# Included once per project() call; only the first one arranges the sweep.
if(NOT DEFINED CRUCIBLE_UNVERSION_DEFERRED)
    set(CRUCIBLE_UNVERSION_DEFERRED ON)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
                   CALL crucible_unversion_directory "${CMAKE_SOURCE_DIR}")
endif()
