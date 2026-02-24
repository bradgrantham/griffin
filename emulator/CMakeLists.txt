cmake_minimum_required(VERSION 3.20)
project(emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)

FetchContent_MakeAvailable(glfw)

add_executable(emulator emulator.cpp)
target_link_libraries(emulator PRIVATE glfw Moira)

add_library(Moira STATIC
   external/Moira/Moira/Moira.cpp
   external/Moira/Moira/MoiraDebugger.cpp
)

target_include_directories(Moira PUBLIC external/Moira/Moira)
target_include_directories(Moira PUBLIC ${PROJECT_SOURCE_DIR}/..)
