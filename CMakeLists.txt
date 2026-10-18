cmake_minimum_required(VERSION 3.16)
# C as well as C++: wayland-scanner emits the protocol marshalling as C.
project(esca-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

# --- xdg-shell -------------------------------------------------------------
# XML that ships with wayland-protocols, turned into a header and a C file by
# the build host's scanner -- hence wayland-native in the recipe, not the
# target's wayland.
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(XDG_SHELL_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
set(XDG_SHELL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")

add_custom_command(
    OUTPUT "${XDG_SHELL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
    DEPENDS "${XDG_SHELL_XML}" VERBATIM)
add_custom_command(
    OUTPUT "${XDG_SHELL_SOURCE}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${XDG_SHELL_SOURCE}"
    DEPENDS "${XDG_SHELL_XML}" VERBATIM)

# --- libesca-ui ------------------------------------------------------------
# The drawing and windowing the applications share. Extracted from
# esca-camview once a second application needed it, rather than copied.
add_library(esca-ui SHARED
    src/window.cpp
    src/render.cpp
    "${XDG_SHELL_SOURCE}"
    "${XDG_SHELL_HEADER}")
target_include_directories(esca-ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(esca-ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(esca-ui PUBLIC PkgConfig::WAYLAND_CLIENT)
set_target_properties(esca-ui PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)

install(TARGETS esca-ui LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/esca DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
