#!/bin/bash
# What a prebuilt runtime is made from, and the name it is published under.
#
#   runtime-recipe.sh BACKEND PLATFORM RUNNER
#
# runtimes.yml builds a module from exactly what this says, so two runs with
# the same recipe make the same module, and one already on an earlier release
# is taken from there rather than built again. That was an hour of CUDA on
# Linux and another on Windows for every release, for a module that had not
# changed since the llama.cpp tag last moved.
#
# Writes the configure flags to flags.txt, one a line, with @SRC@ where the
# checkout's path goes, and prints, for $GITHUB_ENV:
#   ASSET=<the published name>
#   RECIPE=<the recipe's sha256>
set -euo pipefail
backend="$1"
platform="$2"
runner="$3"
here="$(cd "$(dirname "$0")/../.." && pwd)"

sha() {
    if command -v sha256sum > /dev/null 2>&1; then sha256sum | cut -d' ' -f1; else shasum -a 256 | cut -d' ' -f1; fi
}
# A file's content as Git holds it, whatever line endings the checkout gave it.
content() { tr -d '\r' < "$1"; }

tag="$(grep -m1 'CRUCIBLE_LLAMA_TAG' "$here/cmake/CrucibleDependencies.cmake" \
       | sed -E 's/.*CRUCIBLE_LLAMA_TAG[[:space:]]+([^[:space:])]+).*/\1/')"
test -n "$tag"

# CUDA's name says which kernels it has (prebuilt::kCudaKernels), and it is
# built with the CUDA release Crucible fetches cuBLAS for.
cuda=""
suffix=""
if [ "$backend" = cuda ]; then
    cuda="$(grep -m1 'kRelease' "$here/include/crucible/runtime/cuda_libraries.hpp" \
            | sed -E 's/.*"([0-9.]+)".*/\1/')"
    kernels="$(grep -m1 'kCudaKernels = ' "$here/include/crucible/runtime/prebuilt.hpp" \
               | sed -E 's/.*kCudaKernels = ([0-9]+).*/\1/')"
    test -n "$cuda" && test -n "$kernels"
    suffix="-k$kernels"
fi

# The same flags src/runtime/builder.cpp uses, with two differences that come
# from building for somebody else's machine rather than this one:
#
#  - the CUDA architectures are llama.cpp's own list for a build that is not
#    native: kernels for the cards people run (Ampere, Ada, Blackwell) and PTX
#    from Turing up for the rest. The list once began at Ampere, and every RTX
#    20 and GTX 16 failed at its first kernel; prebuilt::kCudaKernels counts
#    which list a download has.
#  - cudart is asked for statically, but ggml-cuda links it and cuBLAS as
#    shared libraries all the same: the module needs libcudart and libcublas
#    beside the driver, which is why the app fetches NVIDIA's libraries with it
#    (src/runtime/cuda_libraries.hpp).
#
# Windows CUDA builds through Ninja rather than Visual Studio. The VS generator
# compiles CUDA through MSBuild, which needs the toolkit's integration files
# inside the Visual Studio installation, and a network install of the toolkit
# does not put them there: cmake finds the toolkit, says so, and then stops at
# "No CUDA toolset found". Ninja calls nvcc directly and needs none of it.
flags=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
       -DGGML_BACKEND_DL=ON -DGGML_NATIVE=OFF
       -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF
       -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF
       -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF
       -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_CURL=OFF
       -DCMAKE_PROJECT_INCLUDE=@SRC@/cmake/CrucibleUnversionHook.cmake)
case "$backend" in
    cpu)    flags+=(-DGGML_CPU=ON -DGGML_CPU_ALL_VARIANTS=ON -DGGML_METAL=OFF) ;;
    vulkan) flags+=(-DGGML_VULKAN=ON -DGGML_CPU=OFF -DGGML_METAL=OFF) ;;
    metal)  flags+=(-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_CPU=OFF) ;;
    cuda)   flags+=(-DGGML_CUDA=ON -DGGML_CPU=OFF -DGGML_METAL=OFF
                    -DCMAKE_CUDA_RUNTIME_LIBRARY=Static) ;;
    *)      echo "no such backend: $backend" >&2; exit 1 ;;
esac
if [ "$backend" = cuda ] && [ "${platform%%-*}" = windows ]; then
    flags+=(-G Ninja)
fi
printf '%s\n' "${flags[@]}" > flags.txt

# The build hook is part of what makes the module; the runner's name stands for
# its compilers, which an image update can move without changing the name, and
# that is accepted: the module is the same llama.cpp built the same way.
recipe="$( { printf '%s\n' "backend=$backend" "platform=$platform" "runner=$runner" \
                           "llama=$tag" "cuda=$cuda" \
                           "hook=$(content "$here/cmake/CrucibleUnversionHook.cmake" | sha)"
             cat flags.txt; } | sha)"

echo "ASSET=runtime-${backend}-${platform}-${tag}${suffix}.tar.gz"
echo "RECIPE=$recipe"
