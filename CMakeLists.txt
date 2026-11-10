cmake_minimum_required(VERSION 3.18)
project(stategate C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(STATEGATE_VIEWER "Build stategate-view (fetches raylib)" ON)

if(MSVC)
  message(FATAL_ERROR "stategate builds on Windows with MinGW-w64 (MSYS2 UCRT64), not MSVC. See README.md, Windows.")
endif()

include(FetchContent)

# libyaml: the system's through pkg-config on Linux and macOS. On Windows, or
# where pkg-config does not find it, built here from the 0.2.5 sources.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT WIN32)
  pkg_check_modules(YAML QUIET IMPORTED_TARGET yaml-0.1)
endif()
if(TARGET PkgConfig::YAML)
  set(SG_YAML PkgConfig::YAML)
else()
  message(STATUS "libyaml: building 0.2.5 from source")
  FetchContent_Declare(libyaml
    GIT_REPOSITORY https://github.com/yaml/libyaml.git
    GIT_TAG 2c891fc7a770e8ba2fec34fc6b545c672beb37e6 # 0.2.5
    SOURCE_SUBDIR not-a-cmake-project) # sources only: its own CMakeLists would install itself
  FetchContent_MakeAvailable(libyaml)
  file(GLOB SG_YAML_SOURCES ${libyaml_SOURCE_DIR}/src/*.c)
  add_library(sgyaml STATIC ${SG_YAML_SOURCES})
  target_include_directories(sgyaml PUBLIC ${libyaml_SOURCE_DIR}/include)
  target_compile_definitions(sgyaml
    PUBLIC YAML_DECLARE_STATIC
    PRIVATE YAML_VERSION_MAJOR=0 YAML_VERSION_MINOR=2 YAML_VERSION_PATCH=5 YAML_VERSION_STRING="0.2.5")
  set(SG_YAML sgyaml)
endif()

if(WIN32)
  # C99 printf (%zu, %lld) on every C runtime, and executables that run
  # outside the MSYS2 shell: no libgcc or winpthread DLLs to carry along.
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
  add_link_options(-static)
endif()

# Model parsing and validation, shared by the CLI and the viewer.
add_library(sgcore STATIC
  src/util.c
  src/platform.c
  src/ydoc.c
  src/model.c
  src/medit.c
  src/validate.c)
target_include_directories(sgcore PUBLIC src)
target_compile_definitions(sgcore PUBLIC _GNU_SOURCE)
target_compile_options(sgcore PRIVATE -Wall -Wextra -Wswitch-enum -Werror=implicit-function-declaration)
target_link_libraries(sgcore PUBLIC ${SG_YAML})
if(WIN32)
  target_link_libraries(sgcore PUBLIC shell32)
endif()

# cJSON 1.7.18 (MIT), vendored for the MCP server.
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

add_executable(stategate
  src/main.c
  src/ai.c
  src/mcp.c
  src/backend.c
  src/gen_rust.c
  src/manifest.c
  src/ticket.c
  src/gate.c)
target_compile_options(stategate PRIVATE -Wall -Wextra -Wswitch-enum -Werror=implicit-function-declaration)
target_link_libraries(stategate PRIVATE sgcore cjson)

# Tests for the canvas editor's YAML edits.
add_executable(medit_test tests/medit_test.c)
target_compile_options(medit_test PRIVATE -Wall -Wextra)
target_link_libraries(medit_test PRIVATE sgcore)

install(TARGETS stategate DESTINATION bin)
# `fsm` is a short alias for the same binary: a symlink, or a copy on
# Windows, where making symlinks needs extra rights.
if(WIN32)
  install(PROGRAMS $<TARGET_FILE:stategate> DESTINATION bin RENAME fsm.exe)
else()
  install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink stategate \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/fsm)")
endif()

if(STATEGATE_VIEWER)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)

  add_executable(stategate-view viewer/main.c viewer/gate_status.c viewer/runner.c)
  target_include_directories(stategate-view PRIVATE viewer)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(stategate-view PRIVATE -Wno-format-truncation)
  endif()
  target_compile_definitions(stategate-view PRIVATE
    SG_RESOURCE_DIR="${CMAKE_INSTALL_PREFIX}/share/stategate"
    SG_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/viewer/resources")
  target_link_libraries(stategate-view PRIVATE sgcore cjson raylib)
  if(NOT WIN32)
    target_link_libraries(stategate-view PRIVATE m)
  endif()
  if(NOT APPLE AND NOT WIN32)
    target_link_libraries(stategate-view PRIVATE util) # forkpty
  endif()
  install(TARGETS stategate-view DESTINATION bin)
  install(FILES viewer/resources/Roboto-Regular.ttf DESTINATION share/stategate)
endif()
