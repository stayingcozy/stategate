cmake_minimum_required(VERSION 3.16)
project(stategate C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

add_executable(stategate
  src/main.c
  src/util.c
  src/ydoc.c
  src/model.c
  src/validate.c
  src/gen_rust.c
  src/manifest.c
  src/ticket.c
  src/gate.c)
target_compile_definitions(stategate PRIVATE _GNU_SOURCE)
target_compile_options(stategate PRIVATE -Wall -Wextra -Wswitch-enum -Werror=implicit-function-declaration)
target_link_libraries(stategate PRIVATE PkgConfig::YAML)

install(TARGETS stategate DESTINATION bin)
# `fsm` is a short alias for the same binary.
install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink stategate \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/fsm)")
