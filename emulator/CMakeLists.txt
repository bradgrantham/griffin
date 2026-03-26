cmake_minimum_required(VERSION 3.20)
project(emulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

add_executable(emulator emulator.cpp)
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
