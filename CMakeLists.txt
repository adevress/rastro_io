cmake_minimum_required(VERSION 3.22)
project(rastro-io VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(RASTRO_STATIC "Link rastro as a fully static binary" OFF)
option(RASTRO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

## Common warnings for every target of the project
list(APPEND RASTRO_COMMON_FLAGS
  "-Wall"
  "-Wextra"
  "-Wshadow"
  "-Wformat-security"
  "-fno-omit-frame-pointer"
)

if(RASTRO_WARNINGS_AS_ERRORS)
  list(APPEND RASTRO_COMMON_FLAGS "-Werror")
endif()

## Fully static binary, if requested
if(RASTRO_STATIC)
  # -static is propagated through the exported helper variable so that the
  # binaries are pure static executables.
  set(RASTRO_STATIC_LINK_FLAGS "-static")
  add_link_options("-static")
  message(STATUS "rastro-io: static binary requested")
endif()

## Dependencies of the whole project
## HighFive and HDF5 will be used by the UVH5 I/O layer.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CASACORE REQUIRED IMPORTED_TARGET casacore)
pkg_check_modules(XTENSOR REQUIRED IMPORTED_TARGET xtensor)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(HighFive REQUIRED)
find_package(cxxopts REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(xtl REQUIRED)

add_subdirectory(src)
