cmake_minimum_required(VERSION 3.20)

project(q2manager VERSION 0.1.0 LANGUAGES CXX)

string(TIMESTAMP Q2MANAGER_BUILD_DATE "%Y-%m-%d")
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE Q2MANAGER_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if (NOT Q2MANAGER_GIT_HASH)
  set(Q2MANAGER_GIT_HASH unknown)
endif()
set(Q2MANAGER_VERSION_STRING "${Q2MANAGER_BUILD_DATE}-${Q2MANAGER_GIT_HASH}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Keep vendored dependency CMake deprecation chatter out of normal configure logs.
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  raygui
  GIT_REPOSITORY https://github.com/raysan5/raygui.git
  GIT_TAG 4.0
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(raylib raygui nlohmann_json)

if (TARGET raylib)
  if (NOT MSVC)
    target_compile_options(raylib PRIVATE -w)
  endif()
endif()

add_executable(q2manager
  src/main.cpp
)

target_include_directories(q2manager PRIVATE
  ${raygui_SOURCE_DIR}/src
)

target_link_libraries(q2manager PRIVATE raylib nlohmann_json::nlohmann_json)

target_compile_definitions(q2manager PRIVATE Q2MANAGER_VERSION="${Q2MANAGER_VERSION_STRING}")

if (WIN32)
  target_compile_definitions(q2manager PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if (MSVC)
  target_compile_options(q2manager PRIVATE /W4)
else()
  target_compile_options(q2manager PRIVATE -Wall -Wextra -Wpedantic)
endif()
