## Reusable library with all the MSv2 reading logic.
add_library(rastro_msv2
  measurement_set.cpp
)

## Expose the source root (not the msv2 directory) so that consumers include
## this format as <msv2/measurement_set.hpp>. Neither format's headers leak
## into the other; the shared contract lives in src/measurement_set_format.hpp.
target_include_directories(rastro_msv2
  PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(rastro_msv2 PRIVATE ${RASTRO_COMMON_FLAGS})

target_link_libraries(rastro_msv2
  PUBLIC
    PkgConfig::CASACORE
)
