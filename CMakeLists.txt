cmake_minimum_required(VERSION 3.16)
project(stategate C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(STATEGATE_VIEWER "Build stategate-view (fetches raylib)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

# Model parsing and validation, shared by the CLI and the viewer.
add_library(sgcore STATIC
  src/util.c
  src/ydoc.c
  src/model.c
  src/validate.c)
target_include_directories(sgcore PUBLIC src)
target_compile_definitions(sgcore PUBLIC _GNU_SOURCE)
target_compile_options(sgcore PRIVATE -Wall -Wextra -Wswitch-enum -Werror=implicit-function-declaration)
target_link_libraries(sgcore PUBLIC PkgConfig::YAML)

add_executable(stategate
  src/main.c
  src/gen_rust.c
  src/manifest.c
  src/ticket.c
  src/gate.c)
target_compile_options(stategate PRIVATE -Wall -Wextra -Wswitch-enum -Werror=implicit-function-declaration)
target_link_libraries(stategate PRIVATE sgcore)

install(TARGETS stategate DESTINATION bin)
# `fsm` is a short alias for the same binary.
install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink stategate \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/fsm)")

if(STATEGATE_VIEWER)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)

  add_executable(stategate-view viewer/main.c viewer/gate_status.c)
  target_include_directories(stategate-view PRIVATE viewer)
  target_compile_definitions(stategate-view PRIVATE
    SG_RESOURCE_DIR="${CMAKE_INSTALL_PREFIX}/share/stategate"
    SG_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/viewer/resources")
  target_link_libraries(stategate-view PRIVATE sgcore raylib m)
  install(TARGETS stategate-view DESTINATION bin)
  install(FILES viewer/resources/Roboto-Regular.ttf DESTINATION share/stategate)
endif()
