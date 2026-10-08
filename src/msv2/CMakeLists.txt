## Reusable library with all the MSv2 reading logic.
add_library(rastro_msv2
  measurement_set.cpp
  msv2_reader.cpp
  msv2_writer.cpp
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
    rastro_meas
    PkgConfig::CASACORE
)

## Self-contained MSv2 writer/reader round-trip test.
add_executable(rastro_msv2_roundtrip_test tests/msv2_roundtrip_test.cpp)
target_link_libraries(rastro_msv2_roundtrip_test PRIVATE rastro_msv2)
target_compile_options(rastro_msv2_roundtrip_test PRIVATE ${RASTRO_COMMON_FLAGS})
add_test(NAME rastro_msv2_roundtrip COMMAND rastro_msv2_roundtrip_test)
