## Reusable library with all the MSv2 reading logic.
add_library(rastro_msv2
  measurement_set_summary.cpp
)

target_include_directories(rastro_msv2
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(rastro_msv2 PRIVATE ${RASTRO_COMMON_FLAGS})

target_link_libraries(rastro_msv2
  PUBLIC
    PkgConfig::CASACORE
)
