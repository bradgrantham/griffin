cmake_minimum_required(VERSION 3.20)
project(emulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build: the interpreter core is hot, and an
# unspecified build type (plain -O0) runs the guest ~3x slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.14
)
FetchContent_MakeAvailable(SDL3)

# Single source of truth: regenerate griffin.generated.* from griffin.yml by
# delegating to the root Makefile (keyed to griffin.yml/codegen.py), the same
# way the subdir Makefiles do.  emulator.cpp includes ../griffin.generated.h,
# so the emulator rebuilds whenever the YAML changes.
set(GRIFFIN_ROOT ${PROJECT_SOURCE_DIR}/..)
add_custom_command(
    OUTPUT ${GRIFFIN_ROOT}/griffin.generated.h
    COMMAND make -C ${GRIFFIN_ROOT} codegen
    DEPENDS ${GRIFFIN_ROOT}/griffin.yml ${GRIFFIN_ROOT}/codegen.py
    COMMENT "Regenerating griffin.generated.* from griffin.yml"
    VERBATIM
)
add_custom_target(griffin_generated DEPENDS ${GRIFFIN_ROOT}/griffin.generated.h)

add_executable(emulator emulator.cpp)
add_dependencies(emulator griffin_generated)
target_link_libraries(emulator PRIVATE SDL3::SDL3 Moira)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(emulator PRIVATE util)
endif()

add_library(Moira STATIC
   external/Moira/Moira/Moira.cpp
   external/Moira/Moira/MoiraDebugger.cpp
)
target_include_directories(Moira PUBLIC external/Moira/Moira)
target_include_directories(Moira PUBLIC ${PROJECT_SOURCE_DIR}/..)
