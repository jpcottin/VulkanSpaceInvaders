cmake_minimum_required(VERSION 3.22.1)
project(spaceinvaders LANGUAGES C CXX)

# --- Locate glslc (ships inside the NDK) and compile GLSL -> SPIR-V C arrays ---
file(GLOB GLSLC_CANDIDATES
    "${ANDROID_NDK}/shader-tools/*/glslc"
    "${ANDROID_NDK}/shader-tools/*/glslc.exe")
if(GLSLC_CANDIDATES)
    list(GET GLSLC_CANDIDATES 0 GLSLC)
else()
    message(FATAL_ERROR "glslc not found under ${ANDROID_NDK}/shader-tools")
endif()

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(compile_shader STAGE_FILE OUT_HEADER)
    add_custom_command(
        OUTPUT "${GEN_DIR}/${OUT_HEADER}"
        COMMAND "${GLSLC}" -mfmt=c -O
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${STAGE_FILE}"
                -o "${GEN_DIR}/${OUT_HEADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${STAGE_FILE}"
        COMMENT "Compiling ${STAGE_FILE} -> ${OUT_HEADER}"
        VERBATIM)
endfunction()

compile_shader(shape.vert shape_vert.spv.h)
compile_shader(shape.frag shape_frag.spv.h)
add_custom_target(shaders DEPENDS
    "${GEN_DIR}/shape_vert.spv.h"
    "${GEN_DIR}/shape_frag.spv.h")

# --- native_app_glue (lifecycle + input plumbing for NativeActivity) ---
add_library(native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue")

# --- Oboe audio library (auto-selects AAudio on API 26+, OpenSL ES below) ---
include(FetchContent)
# Pinned to the commit behind tag 1.9.3 (a tag can be moved, a SHA cannot),
# fetched as a 6 MB tarball with a content hash rather than a git clone.
FetchContent_Declare(oboe
    URL      https://github.com/google/oboe/archive/b15f5e39c01a7ada306d959e5129620b145fb8b4.tar.gz
    URL_HASH SHA256=bc605cc0cee75126ab22443a4b4eb3d2b69d0bdc3caf07450659a11798fcb4a1)
FetchContent_MakeAvailable(oboe)

# --- The game library (loaded by NativeActivity as libspaceinvaders.so) ---
add_library(spaceinvaders SHARED
    main.cpp
    vk_renderer.cpp
    game.cpp
    audio.cpp
    glasses.cpp)

add_dependencies(spaceinvaders shaders)
target_include_directories(spaceinvaders PRIVATE "${GEN_DIR}")
# -Wno-missing-field-initializers: the `VkFoo ci{VK_STRUCTURE_TYPE_...}` idiom
# deliberately leaves the rest of every create-info struct zeroed.
target_compile_options(spaceinvaders PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# Oboe's public headers carry their own unused-parameter warnings.
target_include_directories(spaceinvaders SYSTEM PRIVATE
    $<TARGET_PROPERTY:oboe,INTERFACE_INCLUDE_DIRECTORIES>)

# Keep ANativeActivity_onCreate (provided by glue) from being stripped.
target_link_options(spaceinvaders PRIVATE "-u" "ANativeActivity_onCreate")

target_link_libraries(spaceinvaders
    oboe
    native_app_glue
    vulkan
    android
    log)

# --- Unit tests (Google Test from NDK, runs as an Android executable) ---
# Debug only: the Game test hooks are compiled out under NDEBUG, so building
# game_tests in a release variant (RelWithDebInfo) fails.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(GTEST_ROOT "${ANDROID_NDK}/sources/third_party/googletest")

    add_library(gtest STATIC
        "${GTEST_ROOT}/src/gtest-all.cc"
        "${GTEST_ROOT}/src/gtest_main.cc")
    target_include_directories(gtest PUBLIC  "${GTEST_ROOT}/include"
                                     PRIVATE "${GTEST_ROOT}")

    add_executable(game_tests
        test/game_test.cpp
        test/audio_stub.cpp
        game.cpp)
    target_include_directories(game_tests PRIVATE
        "${GTEST_ROOT}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(game_tests PRIVATE -Wall -Wextra)
    target_link_libraries(game_tests gtest log)
endif()
